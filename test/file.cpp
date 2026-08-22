// File IO through the ring - the ops the engine thread made possible.
// select(2) calls every regular file ready, always, so a file read has
// to be run by somebody, blocking; the whole point is that the somebody
// is never the caller. This test proves the caller stayed free: it
// submits and then waits on ring_fd, the ring's own descriptor.
#include <poll.h>
#include <liburing.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>

static const char kPath[] = "/tmp/slipstream-file-test.txt";
static const char kBody[] = "the engine read this";

int main() {
  alarm(60);
  const size_t body_len = sizeof(kBody) - 1;
  {  // the fixture, written the boring way: it is not what is under test
    int fd = ::open(kPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { std::printf("fixture: cannot create %s\n", kPath); return 1; }
    if (::write(fd, kBody, body_len) != static_cast<ssize_t>(body_len)) return 1;
    ::close(fd);
  }

  struct io_uring r;
  if (io_uring_queue_init(16, &r, 0) != 0) return 1;
  struct io_uring_cqe* c = nullptr;

  // openat: a blocking syscall, run on the engine, answered as a cqe.
  struct io_uring_sqe* s = io_uring_get_sqe(&r);
  io_uring_prep_openat(s, AT_FDCWD, kPath, O_RDONLY, 0);
  io_uring_sqe_set_data64(s, 1);
  io_uring_submit(&r);
  if (io_uring_wait_cqe(&r, &c) != 0 || io_uring_cqe_get_data64(c) != 1) return 2;
  const int fd = c->res;
  if (fd < 0) { std::printf("openat: %s\n", std::strerror(-fd)); return 2; }
  io_uring_cqe_seen(&r, c);
  std::printf("openat through the ring -> fd %d\n", fd);

#ifdef STATX_BASIC_STATS
  {
    struct statx stx;
    std::memset(&stx, 0, sizeof(stx));
    s = io_uring_get_sqe(&r);
    io_uring_prep_statx(s, AT_FDCWD, kPath, 0, STATX_SIZE, &stx);
    io_uring_sqe_set_data64(s, 2);
    io_uring_submit(&r);
    if (io_uring_wait_cqe(&r, &c) != 0 || io_uring_cqe_get_data64(c) != 2) return 3;
    if (c->res != 0) { std::printf("statx: %s\n", std::strerror(-c->res)); return 3; }
    io_uring_cqe_seen(&r, c);
    if (stx.stx_size != body_len) {
      std::printf("statx: size %llu, expected %zu\n", (unsigned long long)stx.stx_size, body_len);
      return 3;
    }
    std::printf("statx through the ring -> %llu bytes\n", (unsigned long long)stx.stx_size);
  }
#else
  std::printf("statx: no STATX_BASIC_STATS on this host, op answers -EOPNOTSUPP by contract\n");
#endif

  // read: submitted, and then WAITED FOR ON ring_fd. Not one io_uring_*
  // call runs between the submit and the wakeup - the completion is the
  // engine's work, arriving on the ring's own pollable descriptor.
  {
    char buf[64];
    std::memset(buf, 0, sizeof(buf));
    s = io_uring_get_sqe(&r);
    io_uring_prep_read(s, fd, buf, static_cast<unsigned>(sizeof(buf)), 0);
    io_uring_sqe_set_data64(s, 3);
    io_uring_submit(&r);

    struct pollfd pfd;
    pfd.fd = r.ring_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    const int prc = poll(&pfd, 1, 5000);
    if (prc != 1 || !(pfd.revents & POLLIN)) {
      std::printf("read: poll on ring_fd returned %d, revents %d\n", prc, pfd.revents);
      return 4;
    }
    if (io_uring_peek_cqe(&r, &c) != 0 || io_uring_cqe_get_data64(c) != 3) return 4;
    if (c->res != static_cast<int>(body_len)) {
      std::printf("read: %d bytes, expected %zu\n", c->res, body_len);
      return 4;
    }
    if (std::memcmp(buf, kBody, body_len) != 0) { std::printf("read: wrong bytes\n"); return 4; }
    io_uring_cqe_seen(&r, c);
    std::printf("read through the ring, awaited on ring_fd -> \"%s\"\n", buf);
  }

  // THE POOL, and what it is for: a blocking read that will not answer
  // for a while must not hold up anything else. The read is submitted
  // FIRST, on a pipe with nothing in it; the socket op submitted after
  // it has to complete first, which can only happen if the read is
  // sitting in a work thread and not in the engine's loop.
  {
    int pfds[2], sv[2];
    if (pipe(pfds) != 0 || socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return 6;
    char slow[16];
    std::memset(slow, 0, sizeof(slow));

    s = io_uring_get_sqe(&r);
    io_uring_prep_read(s, pfds[0], slow, 4, static_cast<uint64_t>(-1));  // no data yet: blocks
    io_uring_sqe_set_data64(s, 10);
    s = io_uring_get_sqe(&r);
    io_uring_prep_poll_add(s, sv[0], POLLIN);
    io_uring_sqe_set_data64(s, 11);
    io_uring_submit(&r);

    if (write(sv[1], "x", 1) != 1) return 6;
    if (io_uring_wait_cqe(&r, &c) != 0) return 6;
    if (io_uring_cqe_get_data64(c) != 11) {
      std::printf("pool: first completion was %llu, expected the socket (11) - the engine was "
                  "stuck on the read\n",
                  (unsigned long long)io_uring_cqe_get_data64(c));
      return 6;
    }
    io_uring_cqe_seen(&r, c);
    std::printf("socket completion overtook a blocked file read: ok\n");

    if (write(pfds[1], "done", 4) != 4) return 6;
    if (io_uring_wait_cqe(&r, &c) != 0) return 6;
    if (io_uring_cqe_get_data64(c) != 10 || c->res != 4) {
      std::printf("pool: the read answered %llu/%d\n",
                  (unsigned long long)io_uring_cqe_get_data64(c), c->res);
      return 6;
    }
    io_uring_cqe_seen(&r, c);
    close(pfds[0]);
    close(pfds[1]);
    close(sv[0]);
    close(sv[1]);
  }

  // A chain whose first member is BLOCKING still cancels correctly: the
  // engine holds the queue until the worker's result is back, so the nop
  // behind a failing statx must never run.
  {
    struct statx stx;
    std::memset(&stx, 0, sizeof(stx));
    s = io_uring_get_sqe(&r);
    io_uring_prep_statx(s, AT_FDCWD, "/nonexistent/slipstream", 0, 0, &stx);
    s->flags |= IOSQE_IO_LINK;
    io_uring_sqe_set_data64(s, 20);
    s = io_uring_get_sqe(&r);
    io_uring_prep_nop(s);
    io_uring_sqe_set_data64(s, 21);
    io_uring_submit_and_wait(&r, 2);
    if (io_uring_peek_cqe(&r, &c) != 0 || io_uring_cqe_get_data64(c) != 20 || c->res >= 0) {
      std::printf("link: expected a failing statx first\n");
      return 7;
    }
    io_uring_cqe_seen(&r, c);
    if (io_uring_peek_cqe(&r, &c) != 0 || io_uring_cqe_get_data64(c) != 21 || c->res != -ECANCELED) {
      std::printf("link: the op behind the failing blocking one was not cancelled\n");
      return 7;
    }
    io_uring_cqe_seen(&r, c);
    std::printf("linked chain across a work thread: ordered and cancelled: ok\n");
  }

  // close and unlink go through the ring too, so the descriptor dies on
  // the engine and not underneath it.
  s = io_uring_get_sqe(&r);
  io_uring_prep_close(s, fd);
  io_uring_sqe_set_data64(s, 4);
  s = io_uring_get_sqe(&r);
  io_uring_prep_unlink(s, kPath, 0);
  io_uring_sqe_set_data64(s, 5);
  io_uring_submit_and_wait(&r, 2);
  for (int i = 0; i < 2; i++) {
    if (io_uring_peek_cqe(&r, &c) != 0) { std::printf("teardown: missing cqe %d\n", i); return 5; }
    if (c->res < 0) { std::printf("teardown %d: %s\n", i, std::strerror(-c->res)); return 5; }
    io_uring_cqe_seen(&r, c);
  }

  io_uring_queue_exit(&r);
  std::printf("file io: ok\n");
  return 0;
}
