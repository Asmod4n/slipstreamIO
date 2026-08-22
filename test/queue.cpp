// <poll.h> explicitly: against real liburing nothing pulls it in;
// our own liburing.h only happens to. The test compiles against both.
#include <poll.h>
#include <liburing.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

static int64_t elapsed_ms(const struct timespec& a, const struct timespec& b) {
  return (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
}

int main() {
  alarm(60);  // a wait that never returns is a FAILURE, not a hung test run

  struct io_uring r;
  if (io_uring_queue_init(64, &r, 0) != 0) return 1;

  // Submission is a handoff to the engine now, so the completion is
  // waited for and not peeked at: against a real ring, peeking straight
  // after submit was always a race this implementation happened to win.
  struct io_uring_sqe* s = io_uring_get_sqe(&r);
  io_uring_prep_nop(s);
  io_uring_sqe_set_data64(s, 42);
  io_uring_submit(&r);
  struct io_uring_cqe* c = nullptr;
  if (io_uring_wait_cqe(&r, &c) != 0) return 1;
  if (io_uring_cqe_get_data64(c) != 42 || c->res != 0) return 2;
  io_uring_cqe_seen(&r, c);

  // (a) An empty queue has nothing that will ever complete, so a short
  // deadline must run out - and take roughly that long, not return
  // instantly - and report -ETIME with the cqe pointer cleared.
  {
    struct __kernel_timespec ts{0, 150000000};  // 150ms
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    struct io_uring_cqe* oc = reinterpret_cast<struct io_uring_cqe*>(1);  // poisoned
    int rc = io_uring_submit_and_wait_timeout(&r, &oc, 1, &ts, nullptr);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (rc != -ETIME) { std::printf("empty queue: expected -ETIME, got %d\n", rc); return 3; }
    if (oc != nullptr) { std::printf("empty queue: cqe_ptr not cleared on timeout\n"); return 3; }
    int64_t ms = elapsed_ms(t0, t1);
    if (ms < 100 || ms > 2000) {
      std::printf("empty queue: waited %lldms, expected roughly 150ms\n", (long long)ms);
      return 3;
    }
  }

  // (b) Five completions, walked two at a time with a partial advance in
  // between: a tick cut off mid-batch must leave the rest in cq, in
  // order and complete, for the next one (#116).
  {
    for (uint64_t i = 0; i < 5; i++) {
      struct io_uring_sqe* ns = io_uring_get_sqe(&r);
      io_uring_prep_nop(ns);
      io_uring_sqe_set_data64(ns, 100 + i);
    }
    io_uring_submit_and_wait(&r, 5);  // the engine ran them; wait for all five

    unsigned head;
    struct io_uring_cqe* cqe;
    unsigned seen = 0;
    io_uring_for_each_cqe(&r, head, cqe) {
      if (io_uring_cqe_get_data64(cqe) != 100 + seen) {
        std::printf("batch: entry %u out of order\n", seen);
        return 4;
      }
      seen++;
      if (seen == 2) break;  // interrupted mid-batch, on purpose
    }
    if (seen != 2) { std::printf("batch: saw %u before the break, expected 2\n", seen); return 4; }
    io_uring_cq_advance(&r, seen);  // partial: only the two walked

    unsigned remaining = 0;
    io_uring_for_each_cqe(&r, head, cqe) {
      if (io_uring_cqe_get_data64(cqe) != 100 + 2 + remaining) {
        std::printf("batch: remainder out of order at %u\n", remaining);
        return 4;
      }
      remaining++;
    }
    if (remaining != 3) { std::printf("batch: %u left, expected 3\n", remaining); return 4; }
    io_uring_cq_advance(&r, remaining);
    if (io_uring_peek_cqe(&r, &c) != -EAGAIN) { std::printf("batch: cq not empty after full advance\n"); return 4; }
  }

  // (c) A completion that arrives well before the deadline must return
  // immediately with it, not sit out the full timeout.
  {
    int fds[2];
    if (pipe(fds) != 0) return 5;
    struct io_uring_sqe* ps = io_uring_get_sqe(&r);
    io_uring_prep_poll_add(ps, fds[0], POLLIN);
    io_uring_sqe_set_data64(ps, 999);
    io_uring_submit(&r);  // parks: nothing readable on fds[0] yet

    pid_t kid = fork();
    if (kid == 0) {
      usleep(30000);  // 30ms - far inside the 2s deadline below
      char b = 'x';
      if (write(fds[1], &b, 1) != 1) _exit(1);
      _exit(0);
    }

    struct __kernel_timespec ts{2, 0};  // 2s
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    struct io_uring_cqe* oc = nullptr;
    int rc = io_uring_submit_and_wait_timeout(&r, &oc, 1, &ts, nullptr);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    int status = 0;
    waitpid(kid, &status, 0);
    close(fds[0]);
    close(fds[1]);

    if (WEXITSTATUS(status) != 0) { std::printf("early completion: peer write failed\n"); return 6; }
    if (rc < 0) { std::printf("early completion: submit_and_wait_timeout returned %d\n", rc); return 6; }
    if (oc == nullptr || io_uring_cqe_get_data64(oc) != 999) {
      std::printf("early completion: missing the poll cqe\n");
      return 6;
    }
    io_uring_cqe_seen(&r, oc);
    int64_t ms = elapsed_ms(t0, t1);
    if (ms > 1000) {
      std::printf("early completion: took %lldms, should have returned once ready\n", (long long)ms);
      return 6;
    }
  }

  // (g) More completions than the ring holds. The engine's backlog takes
  // the overflow and the caller's advance pulls it back in, so nothing
  // is dropped and nothing is reordered - the real ring's semantics, and
  // the reason the CQ is a fixed buffer plus a backlog rather than a
  // queue that grows under the producer.
  {
    const uint64_t kTotal = 5000;  // well past the 4096-entry ring
    uint64_t sent = 0;
    while (sent < kTotal) {
      unsigned batch = 0;
      while (batch < 1000 && sent < kTotal) {
        struct io_uring_sqe* ns = io_uring_get_sqe(&r);
        if (ns == nullptr) break;
        io_uring_prep_nop(ns);
        io_uring_sqe_set_data64(ns, sent);
        sent++;
        batch++;
      }
      io_uring_submit(&r);
    }

    uint64_t seen = 0;
    while (seen < kTotal) {
      io_uring_submit_and_wait(&r, 1);
      unsigned head;
      struct io_uring_cqe* cqe;
      unsigned n = 0;
      io_uring_for_each_cqe(&r, head, cqe) {
        if (io_uring_cqe_get_data64(cqe) != seen + n) {
          std::printf("overflow: entry %llu out of order\n", (unsigned long long)(seen + n));
          return 10;
        }
        n++;
      }
      if (n == 0) { std::printf("overflow: woke with nothing after %llu\n", (unsigned long long)seen); return 10; }
      io_uring_cq_advance(&r, n);
      seen += n;
    }
    std::printf("overflow: %llu completions through a %u-entry ring, in order\n",
                (unsigned long long)seen, 4096u);
  }

  // (d) THE POINT OF THE ENGINE: ring_fd is pollable, and completions
  // happen while the caller is not inside this API at all. A recv is
  // armed and submitted; then the caller blocks in a bare poll() on
  // ring_fd - no io_uring_* call in sight - while a child writes to the
  // socket 30ms later. If poll wakes with POLLIN, the completion was
  // produced by somebody else's thread, which is the whole claim.
  {
    static char pool[4][256];
    int e = 0;
    struct io_uring_buf_ring* br = io_uring_setup_buf_ring(&r, 4, 0, 0, &e);
    if (br == nullptr) { std::printf("pollable: no buf ring (%d)\n", e); return 7; }
    const int mask = io_uring_buf_ring_mask(4);
    for (int i = 0; i < 4; i++) io_uring_buf_ring_add(br, pool[i], 256, i, mask, i);
    io_uring_buf_ring_advance(br, 4);

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return 7;
    struct io_uring_sqe* rs = io_uring_get_sqe(&r);
    io_uring_prep_recv_multishot(rs, sv[0], nullptr, 0, 0);
    rs->flags |= IOSQE_BUFFER_SELECT;
    rs->buf_group = 0;
    io_uring_sqe_set_data64(rs, 777);
    io_uring_submit(&r);

    if (r.ring_fd < 0) { std::printf("pollable: ring_fd is not a descriptor\n"); return 7; }

    pid_t kid = fork();
    if (kid == 0) {
      usleep(30000);
      _exit(write(sv[1], "ping", 4) == 4 ? 0 : 1);
    }

    struct pollfd pfd;
    pfd.fd = r.ring_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int prc = poll(&pfd, 1, 5000);  // no API call: only the ring's own fd
    clock_gettime(CLOCK_MONOTONIC, &t1);
    int status = 0;
    waitpid(kid, &status, 0);
    if (WEXITSTATUS(status) != 0) { std::printf("pollable: peer write failed\n"); return 7; }
    if (prc != 1 || !(pfd.revents & POLLIN)) {
      std::printf("pollable: poll returned %d, revents %d\n", prc, pfd.revents);
      return 7;
    }
    const int64_t ms = elapsed_ms(t0, t1);
    if (ms < 15) {  // it really blocked: the byte was not already there
      std::printf("pollable: poll returned after %lldms, expected ~30\n", (long long)ms);
      return 7;
    }
    if (io_uring_peek_cqe(&r, &c) != 0) { std::printf("pollable: POLLIN but no cqe\n"); return 7; }
    if (io_uring_cqe_get_data64(c) != 777 || c->res != 4) {
      std::printf("pollable: cqe %llu res %d\n", (unsigned long long)io_uring_cqe_get_data64(c),
                  c->res);
      return 7;
    }
    if (!(c->flags & IORING_CQE_F_BUFFER)) { std::printf("pollable: no buffer id\n"); return 7; }
    const unsigned bid = c->flags >> IORING_CQE_BUFFER_SHIFT;
    if (std::memcmp(pool[bid], "ping", 4) != 0) { std::printf("pollable: wrong bytes\n"); return 7; }

    // (e) The other half of the invariant: once the ring has been fully
    // advanced, ring_fd must NOT be readable any more. A byte left in
    // the pipe here is a wakeup the embedder would spin on forever.
    io_uring_cqe_seen(&r, c);
    if (io_uring_peek_cqe(&r, &c) == 0) { std::printf("drained: cq not empty\n"); return 8; }
    pfd.revents = 0;
    if (poll(&pfd, 1, 0) != 0) { std::printf("drained: ring_fd still readable\n"); return 8; }

    close(sv[0]);
    close(sv[1]);
  }

  io_uring_queue_exit(&r);

  // (f) Shutdown with ops still parked must not hang: the engine is
  // sitting in select on a socket nobody will ever write to, and
  // queue_exit has to reach it through the control pipe and join.
  {
    struct io_uring q;
    if (io_uring_queue_init(8, &q, 0) != 0) return 9;
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return 9;
    struct io_uring_sqe* ps = io_uring_get_sqe(&q);
    io_uring_prep_poll_add(ps, sv[0], POLLIN);
    io_uring_sqe_set_data64(ps, 5);
    io_uring_submit(&q);
    usleep(20000);  // let the engine reach select with the op parked
    io_uring_queue_exit(&q);  // the alarm(60) above is what would catch a hang
    close(sv[0]);
    close(sv[1]);
  }

  std::printf("queue: pollable ring_fd, drained pipe, clean shutdown: ok\n");
  return 0;
}
