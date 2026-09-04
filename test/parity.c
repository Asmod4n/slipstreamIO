/* The behavior, not only the API: the kernel answers first and the
 * engine has to say the same thing. Every scenario here runs twice
 * through the SAME liburing calls - once with the kernel answering,
 * once with the engine forced - and the two completion streams must
 * match field for field: user_data, res, flags, count,
 * and order where io_uring promises order (a link chain). Where it does
 * not, completions are compared as a set.
 *
 * This is what the fixed park-forever bug looked like from outside: a
 * read on fd -1 answered -EBADF by the kernel and never at all by the
 * engine. A scenario below holds that door shut.
 *
 * Needs a kernel that allows io_uring to compare against, and says so
 * and skips when there is none. Built and run by test/with_liburing.sh. */

/* struct statx reaches glibc's sys/stat.h only under _GNU_SOURCE; a .c
 * of our own may say so on its first line. */
#define _GNU_SOURCE 1

#include <liburing.h>

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "slipstream_engine.h" /* the backend switch: every one of them is measured */
#include "slipstream_syscall.h"

static int failed;

struct rec {
  unsigned long long user_data;
  int res;
  unsigned flags;
};

/* WHICH CQE flags are the contract. IORING_CQE_F_SOCK_NONEMPTY is a
 * hint the kernel may add to a recv and the engine never claims, so it
 * is masked out by name rather than left to make every recv scenario
 * lie. What is compared: the buffer a recv was given (F_BUFFER and the
 * bid in the top 16 bits) and whether a multishot said it is still
 * armed (F_MORE). */
#define REC_FLAG_MASK (IORING_CQE_F_BUFFER | IORING_CQE_F_MORE | 0xffff0000u)

/* A scenario that finds this kernel does not carry the op says so with
 * this, and the pair is skipped instead of counted as a difference. */
#define NOT_IN_THIS_KERNEL (-2)

/* Which side is running - a scenario needs it only to tell "this kernel
 * has no such op" from "the engine got it wrong". */
static int side_is_engine;

#define REC_MAX 16


/* A new descriptor's NUMBER is allocation order, not behavior - the two
 * sides allocate at different moments, so an fd-yielding op records
 * this token for any success and the scenario proves the fd USABLE
 * instead of comparing its number. */
#define FD_OK 900

/* A scenario preps and submits against the ring it is handed and
 * returns how many completions it collected into out[]. It must behave
 * identically whichever side answers - that is the point. */
typedef int (*scenario_fn)(struct io_uring *ring, struct rec *out);

static int collect(struct io_uring *ring, struct rec *out, int want) {
  int n = 0;
  while (n < want) {
    struct io_uring_cqe *cqe;
    if (io_uring_wait_cqe(ring, &cqe) != 0) break;
    out[n].user_data = cqe->user_data;
    out[n].res = cqe->res;
    out[n].flags = cqe->flags & REC_FLAG_MASK;
    io_uring_cqe_seen(ring, cqe);
    n++;
  }
  return n;
}

/* collect(), but it gives up. A scenario for an op ONE side does not
 * have yet would otherwise wait forever for a CQE that is not coming,
 * and a hang says nothing. A short count is an answer: it is printed
 * beside the other side's. */
static int collect_within(struct io_uring *ring, struct rec *out, int want, unsigned ms) {
  struct __kernel_timespec ts;
  int n = 0;
  ts.tv_sec = ms / 1000;
  ts.tv_nsec = (long long) (ms % 1000) * 1000000;
  while (n < want) {
    struct io_uring_cqe *cqe;
    if (io_uring_wait_cqe_timeout(ring, &cqe, &ts) != 0) break;
    out[n].user_data = cqe->user_data;
    out[n].res = cqe->res;
    out[n].flags = cqe->flags & REC_FLAG_MASK;
    io_uring_cqe_seen(ring, cqe);
    n++;
  }
  return n;
}

static int cmp_rec(const void *a, const void *b) {
  const struct rec *x = a, *y = b;
  if (x->user_data != y->user_data) return x->user_data < y->user_data ? -1 : 1;
  if (x->res != y->res) return x->res - y->res;
  return (int) x->flags - (int) y->flags;
}

static int sc_single_nop(struct io_uring *ring, struct rec *out) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  io_uring_prep_nop(sqe);
  io_uring_sqe_set_data64(sqe, 1);
  io_uring_submit(ring);
  return collect(ring, out, 1);
}

static int sc_recv_waits_for_peer(struct io_uring *ring, struct rec *out) {
  int sp[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) return -1;
  static char buf[8];
  memset(buf, 0, sizeof(buf));
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  io_uring_prep_recv(sqe, sp[0], buf, sizeof(buf), 0);
  io_uring_sqe_set_data64(sqe, 2);
  io_uring_submit(ring);
  if (write(sp[1], "peer", 4) != 4) return -1;
  const int n = collect(ring, out, 1);
  const int data_ok = memcmp(buf, "peer", 4) == 0;
  close(sp[0]);
  close(sp[1]);
  return data_ok ? n : -1;
}

static int sc_recv_had_data(struct io_uring *ring, struct rec *out) {
  int sp[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) return -1;
  if (write(sp[1], "ready", 5) != 5) return -1;
  static char buf[8];
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  io_uring_prep_recv(sqe, sp[0], buf, sizeof(buf), 0);
  io_uring_sqe_set_data64(sqe, 3);
  io_uring_submit(ring);
  const int n = collect(ring, out, 1);
  close(sp[0]);
  close(sp[1]);
  return n;
}

static int sc_send(struct io_uring *ring, struct rec *out) {
  int sp[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) return -1;
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  io_uring_prep_send(sqe, sp[1], "sent", 4, 0);
  io_uring_sqe_set_data64(sqe, 4);
  io_uring_submit(ring);
  const int n = collect(ring, out, 1);
  close(sp[0]);
  close(sp[1]);
  return n;
}

static int sc_file_read_at(struct io_uring *ring, struct rec *out) {
  char path[] = "/tmp/slip-parity-XXXXXX";
  const int fd = mkstemp(path);
  if (fd < 0) return -1;
  unlink(path);
  if (pwrite(fd, "0123456789", 10, 0) != 10) return -1;
  static char buf[4];
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  io_uring_prep_read(sqe, fd, buf, 4, 6);
  io_uring_sqe_set_data64(sqe, 5);
  io_uring_submit(ring);
  const int n = collect(ring, out, 1);
  const int data_ok = memcmp(buf, "6789", 4) == 0;
  close(fd);
  return data_ok ? n : -1;
}

static int sc_read_bad_fd(struct io_uring *ring, struct rec *out) {
  static char buf[4];
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  io_uring_prep_read(sqe, -1, buf, 4, 0);
  io_uring_sqe_set_data64(sqe, 6);
  io_uring_submit(ring);
  return collect(ring, out, 1);
}

static int sc_close_bad_fd(struct io_uring *ring, struct rec *out) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  io_uring_prep_close(sqe, -1);
  io_uring_sqe_set_data64(sqe, 7);
  io_uring_submit(ring);
  return collect(ring, out, 1);
}

/* A link chain in order, and a chain whose first member fails: the
 * kernel cancels the rest with -ECANCELED, in submission order. */
static int sc_chain_ok(struct io_uring *ring, struct rec *out) {
  for (int i = 0; i < 3; i++) {
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    io_uring_prep_nop(sqe);
    io_uring_sqe_set_data64(sqe, 10 + (unsigned) i);
    if (i < 2) io_uring_sqe_set_flags(sqe, IOSQE_IO_LINK);
  }
  io_uring_submit(ring);
  return collect(ring, out, 3);
}

static int sc_chain_fails(struct io_uring *ring, struct rec *out) {
  static char buf[4];
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  io_uring_prep_read(sqe, -1, buf, 4, 0);
  io_uring_sqe_set_data64(sqe, 20);
  io_uring_sqe_set_flags(sqe, IOSQE_IO_LINK);
  sqe = io_uring_get_sqe(ring);
  io_uring_prep_nop(sqe);
  io_uring_sqe_set_data64(sqe, 21);
  io_uring_sqe_set_flags(sqe, IOSQE_IO_LINK);
  sqe = io_uring_get_sqe(ring);
  io_uring_prep_nop(sqe);
  io_uring_sqe_set_data64(sqe, 22);
  io_uring_submit(ring);
  return collect(ring, out, 3);
}

static int submit_one(struct io_uring *ring, struct rec *out) {
  io_uring_submit(ring);
  return collect(ring, out, 1);
}

/* socket -> bind -> listen -> (plain connect) -> accept, every step an
 * op, the accepted descriptor proven usable by a send the peer reads. */
static int sc_socket_lifecycle(struct io_uring *ring, struct rec *out) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  io_uring_prep_socket(sqe, AF_INET, SOCK_STREAM, 0, 0);
  io_uring_sqe_set_data64(sqe, 30);
  if (submit_one(ring, out) != 1 || out[0].res < 0) return 1;
  const int lfd = out[0].res;
  out[0].res = FD_OK;

  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  sqe = io_uring_get_sqe(ring);
  io_uring_prep_bind(sqe, lfd, (struct sockaddr *) &a, sizeof(a));
  io_uring_sqe_set_data64(sqe, 31);
  if (submit_one(ring, out + 1) != 1) return 2;

  sqe = io_uring_get_sqe(ring);
  io_uring_prep_listen(sqe, lfd, 4);
  io_uring_sqe_set_data64(sqe, 32);
  if (submit_one(ring, out + 2) != 1) return 3;

  socklen_t alen = sizeof(a);
  if (getsockname(lfd, (struct sockaddr *) &a, &alen) != 0) return -1;

  /* The accept parks first - nobody has knocked - and the plain connect
   * wakes it. */
  sqe = io_uring_get_sqe(ring);
  io_uring_prep_accept(sqe, lfd, NULL, NULL, 0);
  io_uring_sqe_set_data64(sqe, 33);
  io_uring_submit(ring);
  const int cfd = socket(AF_INET, SOCK_STREAM, 0);
  if (cfd < 0 || connect(cfd, (struct sockaddr *) &a, sizeof(a)) != 0) return -1;
  if (collect(ring, out + 3, 1) != 1 || out[3].res < 0) return 4;
  const int afd = out[3].res;
  out[3].res = FD_OK;

  sqe = io_uring_get_sqe(ring);
  io_uring_prep_send(sqe, afd, "hi", 2, 0);
  io_uring_sqe_set_data64(sqe, 34);
  if (submit_one(ring, out + 4) != 1) return 5;
  char got[4] = { 0 };
  if (read(cfd, got, sizeof(got)) != 2 || memcmp(got, "hi", 2) != 0) return -1;

  close(cfd);
  close(afd);
  close(lfd);
  return 5;
}

/* connect through the ring: to a listening socket, and to a port where
 * nobody listens - loopback answers the second one at once. */
static int sc_connect(struct io_uring *ring, struct rec *out) {
  const int lfd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t alen = sizeof(a);
  if (lfd < 0 || bind(lfd, (struct sockaddr *) &a, sizeof(a)) != 0 || listen(lfd, 1) != 0 ||
      getsockname(lfd, (struct sockaddr *) &a, &alen) != 0)
    return -1;

  const int c1 = socket(AF_INET, SOCK_STREAM, 0);
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  io_uring_prep_connect(sqe, c1, (struct sockaddr *) &a, sizeof(a));
  io_uring_sqe_set_data64(sqe, 40);
  if (submit_one(ring, out) != 1) return 1;

  close(lfd); /* the port is nobody's now */
  const int c2 = socket(AF_INET, SOCK_STREAM, 0);
  sqe = io_uring_get_sqe(ring);
  io_uring_prep_connect(sqe, c2, (struct sockaddr *) &a, sizeof(a));
  io_uring_sqe_set_data64(sqe, 41);
  const int n = submit_one(ring, out + 1);
  close(c1);
  close(c2);
  return n == 1 ? 2 : 1;
}

static int sc_msg_roundtrip(struct io_uring *ring, struct rec *out) {
  int sp[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) return -1;
  static char txt[] = "msg";
  struct iovec iov = { .iov_base = txt, .iov_len = 3 };
  struct msghdr mh;
  memset(&mh, 0, sizeof(mh));
  mh.msg_iov = &iov;
  mh.msg_iovlen = 1;
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  io_uring_prep_sendmsg(sqe, sp[1], &mh, 0);
  io_uring_sqe_set_data64(sqe, 50);
  if (submit_one(ring, out) != 1) return 1;

  static char rbuf[8];
  struct iovec riov = { .iov_base = rbuf, .iov_len = sizeof(rbuf) };
  struct msghdr rmh;
  memset(&rmh, 0, sizeof(rmh));
  rmh.msg_iov = &riov;
  rmh.msg_iovlen = 1;
  sqe = io_uring_get_sqe(ring);
  io_uring_prep_recvmsg(sqe, sp[0], &rmh, 0);
  io_uring_sqe_set_data64(sqe, 51);
  const int n = submit_one(ring, out + 1);
  const int data_ok = memcmp(rbuf, "msg", 3) == 0;
  close(sp[0]);
  close(sp[1]);
  return (n == 1 && data_ok) ? 2 : 1;
}

/* poll_add on quiet data (parks, the peer wakes it) and on data already
 * there - the revents mask is the res and must match bit for bit. */
static int sc_poll_add(struct io_uring *ring, struct rec *out) {
  int sp[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) return -1;
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  io_uring_prep_poll_add(sqe, sp[0], POLLIN);
  io_uring_sqe_set_data64(sqe, 60);
  io_uring_submit(ring);
  if (write(sp[1], "x", 1) != 1) return -1;
  if (collect(ring, out, 1) != 1) return 0;

  sqe = io_uring_get_sqe(ring);
  io_uring_prep_poll_add(sqe, sp[0], POLLIN);
  io_uring_sqe_set_data64(sqe, 61);
  const int n = submit_one(ring, out + 1);
  close(sp[0]);
  close(sp[1]);
  return n == 1 ? 2 : 1;
}

/* A parked poll cancelled by user_data: the target answers -ECANCELED,
 * the cancel answers 0; a cancel that names nothing answers -ENOENT. */
static int sc_cancel(struct io_uring *ring, struct rec *out) {
  int sp[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) return -1;
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  io_uring_prep_poll_add(sqe, sp[0], POLLIN);
  io_uring_sqe_set_data64(sqe, 70);
  io_uring_submit(ring);

  sqe = io_uring_get_sqe(ring);
  io_uring_prep_cancel64(sqe, 70, 0);
  io_uring_sqe_set_data64(sqe, 71);
  io_uring_submit(ring);
  if (collect(ring, out, 2) != 2) return 0;

  sqe = io_uring_get_sqe(ring);
  io_uring_prep_cancel64(sqe, 7777, 0);
  io_uring_sqe_set_data64(sqe, 72);
  const int n = submit_one(ring, out + 2);
  close(sp[0]);
  close(sp[1]);
  return n == 1 ? 3 : 2;
}

static int sc_statx_size(struct io_uring *ring, struct rec *out) {
  char path[] = "/tmp/slip-parity-sx-XXXXXX";
  const int fd = mkstemp(path);
  if (fd < 0) return -1;
  if (pwrite(fd, "12345", 5, 0) != 5) return -1;
  struct statx stx;
  memset(&stx, 0, sizeof(stx));
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  io_uring_prep_statx(sqe, AT_FDCWD, path, 0, STATX_BASIC_STATS, &stx);
  io_uring_sqe_set_data64(sqe, 80);
  const int n = submit_one(ring, out);
  const int size_ok = stx.stx_size == 5;
  close(fd);
  unlink(path);
  return (n == 1 && size_ok) ? 1 : 0;
}

static int sc_unlink_and_open(struct io_uring *ring, struct rec *out) {
  char path[] = "/tmp/slip-parity-un-XXXXXX";
  const int fd = mkstemp(path);
  if (fd < 0) return -1;
  if (pwrite(fd, "keep", 4, 0) != 4) return -1;
  close(fd);

  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  io_uring_prep_openat(sqe, AT_FDCWD, path, O_RDONLY, 0);
  io_uring_sqe_set_data64(sqe, 90);
  if (submit_one(ring, out) != 1 || out[0].res < 0) return 1;
  const int rfd = out[0].res;
  out[0].res = FD_OK;
  char got[4] = { 0 };
  if (read(rfd, got, 4) != 4 || memcmp(got, "keep", 4) != 0) return -1;
  close(rfd);

  sqe = io_uring_get_sqe(ring);
  io_uring_prep_unlinkat(sqe, AT_FDCWD, path, 0);
  io_uring_sqe_set_data64(sqe, 91);
  if (submit_one(ring, out + 1) != 1) return 2;

  /* Gone means gone: the same open must now say -ENOENT. */
  sqe = io_uring_get_sqe(ring);
  io_uring_prep_openat(sqe, AT_FDCWD, path, O_RDONLY, 0);
  io_uring_sqe_set_data64(sqe, 92);
  return submit_one(ring, out + 2) == 1 ? 3 : 2;
}

static int sc_shutdown_means_eof(struct io_uring *ring, struct rec *out) {
  int sp[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) return -1;
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  io_uring_prep_shutdown(sqe, sp[1], SHUT_WR);
  io_uring_sqe_set_data64(sqe, 100);
  const int n = submit_one(ring, out);
  char b;
  const int eof_ok = read(sp[0], &b, 1) == 0;
  close(sp[0]);
  close(sp[1]);
  return (n == 1 && eof_ok) ? 1 : 0;
}

static int sc_wait_times_out(struct io_uring *ring, struct rec *out) {
  struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000000LL };
  struct io_uring_cqe *cqe = NULL;
  const int rc = io_uring_wait_cqe_timeout(ring, &cqe, &ts);
  out[0].user_data = 0xE;
  out[0].res = rc;
  out[0].flags = 0; /* no CQE came, so there are no flags to carry */
  return 1;
}


/* ---- the register family, and the ops that stand on it ----------------
 * A sparse fixed table, a slot a new descriptor lands in, a provided
 * buffer ring a recv picks from, and the socket commands - the four
 * things webmachine's ring does before it serves a byte. */

/* A listener on a fixed SLOT: socket_direct into slot 0, then bind and
 * listen through IOSQE_FIXED_FILE, then close_direct empties the slot.
 * The port is ephemeral and therefore not compared - what is compared
 * is that every stage answered the same on both sides. */
static int sc_direct_listener(struct io_uring *ring, struct rec *out) {
  if (io_uring_register_files_sparse(ring, 4) != 0) return -1;

  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  io_uring_prep_socket_direct(sqe, AF_INET, SOCK_STREAM, 0, 0, 0);
  io_uring_sqe_set_data64(sqe, 200);
  if (submit_one(ring, out) != 1) return -1;

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  sqe = io_uring_get_sqe(ring);
  io_uring_prep_bind(sqe, 0, (struct sockaddr *) &addr, sizeof(addr));
  sqe->flags |= IOSQE_FIXED_FILE;
  io_uring_sqe_set_data64(sqe, 201);
  if (submit_one(ring, out + 1) != 1) return 1;

  sqe = io_uring_get_sqe(ring);
  io_uring_prep_listen(sqe, 0, 8);
  sqe->flags |= IOSQE_FIXED_FILE;
  io_uring_sqe_set_data64(sqe, 202);
  if (submit_one(ring, out + 2) != 1) return 2;

  sqe = io_uring_get_sqe(ring);
  io_uring_prep_close_direct(sqe, 0);
  io_uring_sqe_set_data64(sqe, 203);
  if (submit_one(ring, out + 3) != 1) return 3;
  return 4;
}

/* A slot that was never filled is -EBADF, exactly as the kernel answers
 * a fixed-file op on an empty one. */
static int sc_empty_slot_is_ebadf(struct io_uring *ring, struct rec *out) {
  if (io_uring_register_files_sparse(ring, 4) != 0) return -1;
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  char buf[4];
  io_uring_prep_read(sqe, 2, buf, sizeof(buf), 0);
  sqe->flags |= IOSQE_FIXED_FILE;
  io_uring_sqe_set_data64(sqe, 210);
  return submit_one(ring, out) == 1 ? 1 : -1;
}

/* A recv that picks its own buffer: the CQE carries IORING_CQE_F_BUFFER
 * and the bid in its top bits, and the bytes land in THAT buffer. */
static int sc_recv_buffer_select(struct io_uring *ring, struct rec *out) {
  enum { NBUF = 4, BUFSZ = 64 };
  static char pool[NBUF * BUFSZ];
  int err = 0;
  struct io_uring_buf_ring *br = io_uring_setup_buf_ring(ring, NBUF, 7, 0, &err);
  if (br == NULL) return NOT_IN_THIS_KERNEL;
  const int mask = io_uring_buf_ring_mask(NBUF);
  for (int i = 0; i < NBUF; i++)
    io_uring_buf_ring_add(br, pool + i * BUFSZ, BUFSZ, (unsigned short) i, mask, i);
  io_uring_buf_ring_advance(br, NBUF);

  int sp[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) return -1;
  if (write(sp[1], "hello", 5) != 5) return -1;

  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  io_uring_prep_recv(sqe, sp[0], NULL, 0, 0);
  sqe->flags |= IOSQE_BUFFER_SELECT;
  sqe->buf_group = 7;
  io_uring_sqe_set_data64(sqe, 220);
  const int n = submit_one(ring, out);

  /* The bytes must be in the buffer the CQE named, not merely somewhere. */
  int landed = 0;
  if (n == 1 && out[0].res == 5) {
    const unsigned bid = out[0].flags >> IORING_CQE_BUFFER_SHIFT;
    landed = bid < NBUF && memcmp(pool + bid * BUFSZ, "hello", 5) == 0;
  }
  close(sp[0]);
  close(sp[1]);
  io_uring_free_buf_ring(ring, br, NBUF, 7);
  if (!landed) return -1;
  return n;
}

/* Multishot recv: one CQE per arrival carrying F_MORE, and EOF ends it
 * with a res of 0 and no F_MORE - the kernel's own way of saying the
 * multishot is over. */
static int sc_recv_multishot(struct io_uring *ring, struct rec *out) {
  enum { NBUF = 4, BUFSZ = 64 };
  static char pool[NBUF * BUFSZ];
  int err = 0;
  struct io_uring_buf_ring *br = io_uring_setup_buf_ring(ring, NBUF, 9, 0, &err);
  if (br == NULL) return NOT_IN_THIS_KERNEL;
  const int mask = io_uring_buf_ring_mask(NBUF);
  for (int i = 0; i < NBUF; i++)
    io_uring_buf_ring_add(br, pool + i * BUFSZ, BUFSZ, (unsigned short) i, mask, i);
  io_uring_buf_ring_advance(br, NBUF);

  int sp[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) return -1;
  if (write(sp[1], "one", 3) != 3) return -1;

  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  io_uring_prep_recv_multishot(sqe, sp[0], NULL, 0, 0);
  sqe->flags |= IOSQE_BUFFER_SELECT;
  sqe->buf_group = 9;
  io_uring_sqe_set_data64(sqe, 230);
  io_uring_submit(ring);

  struct io_uring_cqe *cqe = NULL;
  int n = 0;
  if (io_uring_wait_cqe(ring, &cqe) == 0) {
    out[n].user_data = cqe->user_data;
    out[n].res = cqe->res;
    out[n].flags = cqe->flags & REC_FLAG_MASK;
    n++;
    io_uring_cqe_seen(ring, cqe);
  }
  close(sp[1]); /* EOF: the multishot's last word */
  if (io_uring_wait_cqe(ring, &cqe) == 0) {
    out[n].user_data = cqe->user_data;
    out[n].res = cqe->res;
    out[n].flags = cqe->flags & REC_FLAG_MASK;
    n++;
    io_uring_cqe_seen(ring, cqe);
  }
  close(sp[0]);
  io_uring_free_buf_ring(ring, br, NBUF, 9);
  return n;
}

/* SETSOCKOPT and GETSOCKOPT as ring commands. A kernel without
 * SOCKET_URING_OP_* says so, and then there is nothing to compare. */
static int sc_cmd_sockopt(struct io_uring *ring, struct rec *out) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  int on = 1;
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  io_uring_prep_cmd_sock(sqe, SOCKET_URING_OP_SETSOCKOPT, fd, SOL_SOCKET, SO_REUSEADDR, &on,
                         sizeof(on));
  io_uring_sqe_set_data64(sqe, 240);
  int n = submit_one(ring, out);
  if (n == 1 && out[0].res < 0 && !side_is_engine) {
    close(fd);
    return NOT_IN_THIS_KERNEL; /* this kernel does not carry the socket commands */
  }

  int back = 0;
  sqe = io_uring_get_sqe(ring);
  io_uring_prep_cmd_sock(sqe, SOCKET_URING_OP_GETSOCKOPT, fd, SOL_SOCKET, SO_REUSEADDR, &back,
                         sizeof(back));
  io_uring_sqe_set_data64(sqe, 241);
  n += submit_one(ring, out + 1);
  const int read_back = back != 0;
  close(fd);
  return read_back ? n : -1;
}


/* Multishot accept into the fixed table: one CQE per connection, each
 * carrying F_MORE and the SLOT the new descriptor landed in - which is
 * how webmachine learns about every connection it serves. The peers
 * connect BEFORE the accept is submitted, so both sides have the same
 * two waiting and neither has to be timed. */
static int sc_accept_multishot(struct io_uring *ring, struct rec *out) {
  if (io_uring_register_files_sparse(ring, 4) != 0) return -1;
  if (io_uring_register_file_alloc_range(ring, 0, 4) != 0) return -1;

  const int lis = socket(AF_INET, SOCK_STREAM, 0);
  if (lis < 0) return -1;
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t alen = sizeof(addr);
  if (bind(lis, (struct sockaddr *) &addr, sizeof(addr)) != 0 || listen(lis, 8) != 0 ||
      getsockname(lis, (struct sockaddr *) &addr, &alen) != 0) {
    close(lis);
    return -1;
  }

  int cli[2] = { -1, -1 };
  for (int i = 0; i < 2; i++) {
    cli[i] = socket(AF_INET, SOCK_STREAM, 0);
    if (cli[i] < 0 || connect(cli[i], (struct sockaddr *) &addr, sizeof(addr)) != 0) {
      for (int j = 0; j <= i; j++)
        if (cli[j] >= 0) close(cli[j]);
      close(lis);
      return -1;
    }
  }

  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  io_uring_prep_multishot_accept_direct(sqe, lis, NULL, NULL, 0);
  io_uring_sqe_set_data64(sqe, 250);
  io_uring_submit(ring);

  int n = 0;
  for (int i = 0; i < 2; i++) {
    struct io_uring_cqe *cqe = NULL;
    if (io_uring_wait_cqe(ring, &cqe) != 0) break;
    out[n].user_data = cqe->user_data;
    out[n].res = cqe->res;
    out[n].flags = cqe->flags & REC_FLAG_MASK;
    n++;
    io_uring_cqe_seen(ring, cqe);
  }
  close(cli[0]);
  close(cli[1]);
  close(lis);
  return n;
}

/* MSG_RING at its own ring: what a worker pool does to stop a worker,
 * and the one shape that needs no second ring. TWO completions land
 * here - the message and the send's own - and what each carries is the
 * kernel's answer, not ours. */
static int sc_msg_ring_self(struct io_uring *ring, struct rec *out) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  io_uring_prep_msg_ring(sqe, ring->ring_fd, 7, 0x515, 0);
  io_uring_sqe_set_data64(sqe, 40);
  io_uring_submit(ring);
  return collect_within(ring, out, 2, 500);
}

/* A target that is not a ring. */
static int sc_msg_ring_bad_fd(struct io_uring *ring, struct rec *out) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  io_uring_prep_msg_ring(sqe, -1, 1, 0x516, 0);
  io_uring_sqe_set_data64(sqe, 41);
  io_uring_submit(ring);
  return collect_within(ring, out, 1, 500);
}

struct scenario {
  const char *what;
  scenario_fn run;
  int ordered; /* order is part of the contract (a link chain) */
};

static const struct scenario scenarios[] = {
  { "a NOP", sc_single_nop, 0 },
  { "a recv that must wait for the peer", sc_recv_waits_for_peer, 0 },
  { "a recv that had its data already", sc_recv_had_data, 0 },
  { "a send", sc_send, 0 },
  { "a file read at an offset", sc_file_read_at, 0 },
  { "a read on fd -1", sc_read_bad_fd, 0 },
  { "a close on fd -1", sc_close_bad_fd, 0 },
  { "a link chain, in order", sc_chain_ok, 1 },
  { "a failing link head cancels the chain, in order", sc_chain_fails, 1 },
  { "socket/bind/listen/accept, the accepted fd usable", sc_socket_lifecycle, 1 },
  { "connect to a listener, and to a dead port", sc_connect, 1 },
  { "sendmsg and recvmsg round a message", sc_msg_roundtrip, 1 },
  { "poll_add parked and woken, and already-ready", sc_poll_add, 1 },
  { "cancel takes a parked poll; a stranger is -ENOENT", sc_cancel, 0 },
  { "statx sees the size just written", sc_statx_size, 1 },
  { "openat reads, unlinkat removes, reopen says so", sc_unlink_and_open, 1 },
  { "shutdown SHUT_WR reads as EOF at the peer", sc_shutdown_means_eof, 1 },
  { "a wait with nothing coming times out", sc_wait_times_out, 1 },
  { "socket_direct/bind/listen/close_direct on a slot", sc_direct_listener, 1 },
  { "a fixed-file op on an empty slot is -EBADF", sc_empty_slot_is_ebadf, 0 },
  { "a recv picks a provided buffer, and fills THAT one", sc_recv_buffer_select, 0 },
  { "multishot recv: F_MORE per arrival, EOF ends it", sc_recv_multishot, 1 },
  { "setsockopt and getsockopt as ring commands", sc_cmd_sockopt, 1 },
  { "multishot accept fills slots, one F_MORE CQE each", sc_accept_multishot, 1 },
  { "msg_ring at its own ring: the message and the send", sc_msg_ring_self, 0 },
  { "msg_ring at a target that is not a ring", sc_msg_ring_bad_fd, 0 },
};

static int run_side(const struct scenario *sc, int engine, struct rec *out) {
  slipstream_syscall_set_engine(engine);
  side_is_engine = engine;
  struct io_uring ring;
  if (io_uring_queue_init(8, &ring, 0) != 0) return -1;
  const int n = sc->run(&ring, out);
  io_uring_queue_exit(&ring);
  return n;
}

/* EVERY backend is measured, not the platform's favourite. What the
 * engine answers has to be what the kernel answers no matter which
 * readiness or completion machinery is underneath - a backend that is
 * only ever exercised by hand-written expectations is a backend whose
 * divergence nobody would notice. The ones this build does not carry
 * refuse the name and are passed over. */
static const char *const backends[] = { "select", "epoll", "kqueue", "dispatch", "iocp" };

static int same_stream(const struct scenario *sc, const struct rec *a, int na,
                       const struct rec *b, int nb) {
  if (na < 0 || na != nb) return 0;
  struct rec x[REC_MAX], y[REC_MAX];
  memcpy(x, a, (size_t) na * sizeof(*a));
  memcpy(y, b, (size_t) nb * sizeof(*b));
  if (!sc->ordered) {
    qsort(x, (size_t) na, sizeof(*x), cmp_rec);
    qsort(y, (size_t) nb, sizeof(*y), cmp_rec);
  }
  for (int i = 0; i < na; i++) {
    if (x[i].user_data != y[i].user_data || x[i].res != y[i].res || x[i].flags != y[i].flags)
      return 0;
  }
  return 1;
}

int main(void) {
  /* The kernel first: without one that answers, there is nothing to
   * compare against. */
  slipstream_syscall_set_engine(0);
  struct io_uring probe;
  if (io_uring_queue_init(2, &probe, 0) != 0) {
    printf("parity: this kernel refuses io_uring - nothing to compare against, skipped\n");
    return 0;
  }
  io_uring_queue_exit(&probe);

  const unsigned count = sizeof(scenarios) / sizeof(scenarios[0]);

  /* The kernel's answers first, once - they do not depend on which
   * backend the engine would have used. */
  static struct rec kernel[64][REC_MAX];
  int nk[64];
  for (unsigned i = 0; i < count; i++) nk[i] = run_side(&scenarios[i], 0, kernel[i]);

  for (unsigned b = 0; b < sizeof(backends) / sizeof(backends[0]); b++) {
    if (slipstream_engine_backend_set(backends[b]) != 0) {
      printf("  %-8s not carried in this build - skipped\n", backends[b]);
      continue;
    }
    printf("  %s:\n", backends[b]);
    for (unsigned i = 0; i < count; i++) {
      if (nk[i] == NOT_IN_THIS_KERNEL) {
        printf("    %-46s skipped, this kernel has no such op\n", scenarios[i].what);
        continue;
      }
      struct rec engine[REC_MAX];
      const int ne = run_side(&scenarios[i], 1, engine);
      const int same = same_stream(&scenarios[i], kernel[i], nk[i], engine, ne);
      printf("    %-46s %s\n", scenarios[i].what, same ? "same as the kernel" : "DIVERGED");
      if (!same) {
        failed = 1;
        for (int j = 0; j < (nk[i] > ne ? nk[i] : ne); j++) {
          printf("      [%d] kernel", j);
          if (j < nk[i])
            printf(" data=%llu res=%d flags=0x%x", kernel[i][j].user_data, kernel[i][j].res,
                   kernel[i][j].flags);
          else printf(" (nothing)");
          printf("  engine");
          if (j < ne)
            printf(" data=%llu res=%d flags=0x%x", engine[j].user_data, engine[j].res,
                   engine[j].flags);
          else printf(" (nothing)");
          printf("\n");
        }
      }
    }
  }

  printf(failed ? "parity: DIVERGED from the kernel\n" : "parity: the engine behaves like the kernel\n");
  return failed;
}
