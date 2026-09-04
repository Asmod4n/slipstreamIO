/* Every readiness backend the platform carries, driven through the
 * same three scenes - the ones THE ONE RULE is made of:
 *
 *   1. a NOP completes inline
 *   2. a recv on an empty socket parks: enter comes straight back with
 *      the CQ still empty, and a later write on the peer completes it
 *   3. a wait with an EXT_ARG timeout on an idle ring answers -ETIME,
 *      and only after the timeout has actually passed
 *
 * The engine API is driven directly - no liburing - so this same file
 * runs on every platform the engine compiles on. Backends this
 * platform does not carry are refused by name with -EINVAL, and the
 * scene says so and moves on. */
#define _DEFAULT_SOURCE 1

#include "slipstream_engine.h"

#include <liburing/io_uring.h> /* the carried liburing's ABI, every platform */

#include <errno.h>
#include <stdint.h>

/* FreeBSD's errno.h hides the name under -std=c11 - the value and the
 * reasoning live in engine_internal.h. */
#ifndef ETIME
#define ETIME 60
#endif
#include <stdio.h>
#include <string.h>
#include <time.h>

/* The scenes speak sockets; how a pair is made and spoken to is the one
 * platform difference this file carries. */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <fcntl.h>
#include <linux/stat.h>
#ifndef AT_FDCWD
#define AT_FDCWD (-100)
#endif
typedef SOCKET SOCKET_T;
static int pair_of_streams(int sp[2]) {
  SOCKET l = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  int alen = sizeof(a);
  if (l == INVALID_SOCKET || bind(l, (struct sockaddr *) &a, sizeof(a)) != 0 ||
      listen(l, 1) != 0 || getsockname(l, (struct sockaddr *) &a, &alen) != 0)
    return -1;
  SOCKET c = socket(AF_INET, SOCK_STREAM, 0);
  if (c == INVALID_SOCKET || connect(c, (struct sockaddr *) &a, sizeof(a)) != 0) return -1;
  SOCKET s = accept(l, NULL, NULL);
  closesocket(l);
  if (s == INVALID_SOCKET) return -1;
  sp[0] = (int) s;
  sp[1] = (int) c;
  return 0;
}
static int peer_write(int fd, const void *buf, unsigned len) {
  return send((SOCKET) fd, buf, (int) len, 0);
}
static int peer_read(int fd, void *buf, unsigned len) {
  return recv((SOCKET) fd, buf, (int) len, 0);
}
static void sock_close(int fd) { closesocket((SOCKET) fd); }
#else
#include <fcntl.h>
#include <linux/stat.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int SOCKET_T;
static int pair_of_streams(int sp[2]) { return socketpair(AF_UNIX, SOCK_STREAM, 0, sp); }
static int peer_write(int fd, const void *buf, unsigned len) {
  return (int) write(fd, buf, len);
}
static int peer_read(int fd, void *buf, unsigned len) {
  return (int) read(fd, buf, len);
}
static void sock_close(int fd) { close(fd); }
#endif

static int failed;
static void check(int ok, const char *what) {
  printf("    %-52s %s\n", what, ok ? "ok" : "FAILED");
  if (!ok) failed = 1;
}

struct drv {
  int fd;
  struct io_uring_params p;
  unsigned char *sqr, *cqr;
  struct io_uring_sqe *sqes;
};

static int ring_open_n(struct drv *d, unsigned entries) {
  memset(d, 0, sizeof(*d));
  d->fd = slipstream_engine_setup(entries, &d->p);
  if (d->fd < 0) return d->fd;
  d->sqr = slipstream_engine_mmap(d->p.sq_off.array + d->p.sq_entries * sizeof(unsigned),
                                  d->fd, IORING_OFF_SQ_RING);
  d->cqr = slipstream_engine_mmap(d->p.cq_off.cqes + d->p.cq_entries * sizeof(struct io_uring_cqe),
                                  d->fd, IORING_OFF_CQ_RING);
  d->sqes = slipstream_engine_mmap(d->p.sq_entries * sizeof(struct io_uring_sqe),
                                   d->fd, IORING_OFF_SQES);
  return (d->sqr && d->cqr && d->sqes) ? 0 : -1;
}

static int ring_open(struct drv *d) { return ring_open_n(d, 4); }

static struct io_uring_sqe *push(struct drv *d, __u8 opcode, int fd, void *addr,
                                 unsigned len, __u64 off, __u64 user_data) {
  unsigned *tail = (unsigned *) (d->sqr + d->p.sq_off.tail);
  unsigned *arr = (unsigned *) (d->sqr + d->p.sq_off.array);
  const unsigned mask = *(unsigned *) (d->sqr + d->p.sq_off.ring_mask);
  const unsigned i = *tail & mask;
  struct io_uring_sqe *s = &d->sqes[i];
  memset(s, 0, sizeof(*s));
  s->opcode = opcode;
  s->fd = fd;
  s->addr = (__u64) (uintptr_t) addr;
  s->len = len;
  s->off = off;
  s->user_data = user_data;
  arr[i] = i;
  __atomic_store_n(tail, *tail + 1, __ATOMIC_RELEASE);
  return s;
}

static unsigned cq_ready(struct drv *d) {
  return __atomic_load_n((unsigned *) (d->cqr + d->p.cq_off.tail), __ATOMIC_ACQUIRE) -
         *(unsigned *) (d->cqr + d->p.cq_off.head);
}

static struct io_uring_cqe *cq_pop(struct drv *d) {
  unsigned *head = (unsigned *) (d->cqr + d->p.cq_off.head);
  const unsigned mask = *(unsigned *) (d->cqr + d->p.cq_off.ring_mask);
  struct io_uring_cqe *cqes = (struct io_uring_cqe *) (d->cqr + d->p.cq_off.cqes);
  struct io_uring_cqe *c = &cqes[*head & mask];
  __atomic_store_n(head, *head + 1, __ATOMIC_RELEASE);
  return c;
}

static long long ms_now(void) {
#ifdef _WIN32
  return (long long) GetTickCount64();
#else
  struct timespec t;
  timespec_get(&t, TIME_UTC);
  return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
#endif
}

static void scenes(const char *name) {
  const int rc = slipstream_engine_backend_set(name);
  if (rc == -EINVAL) {
    printf("  %s: not carried on this platform - skipped\n", name);
    return;
  }
  printf("  %s:\n", name);
  check(rc == 0, "the backend can be chosen by name");
  check(strcmp(slipstream_engine_backend_name(), name) == 0, "and says so when asked");

  struct drv d;
  check(ring_open(&d) == 0, "a ring comes up on it");

  /* 1: inline. */
  push(&d, IORING_OP_NOP, -1, NULL, 0, 0, 0x101);
  int e = slipstream_engine_enter(d.fd, 1, 1, IORING_ENTER_GETEVENTS, NULL, 0);
  check(e == 1 && cq_ready(&d) == 1, "a NOP completes inline");
  check(cq_pop(&d)->user_data == 0x101, "with its user_data");

  /* 2: the rule. A recv with nothing to read must not hold enter. */
  int sp[2] = { -1, -1 };
  check(pair_of_streams(sp) == 0, "a socketpair exists");
  char buf[8] = { 0 };
  push(&d, IORING_OP_RECV, sp[0], buf, sizeof(buf), 0, 0x202);
  const long long before = ms_now();
  e = slipstream_engine_enter(d.fd, 1, 0, 0, NULL, 0);
  check(e == 1 && ms_now() - before < 1000, "enter returns with the recv still open");
  check(cq_ready(&d) == 0, "and the CQ is still empty - the op is pending");
  check(peer_write(sp[1], "wake", 4) == 4, "the peer writes");
  e = slipstream_engine_enter(d.fd, 0, 1, IORING_ENTER_GETEVENTS, NULL, 0);
  struct io_uring_cqe *c = cq_pop(&d);
  check(e == 0 && c->user_data == 0x202 && c->res == 4 && memcmp(buf, "wake", 4) == 0,
        "the write completes the parked recv");

  /* 2b: the socket lifecycle through the ring. Plain calls that answer
   * at once - what is proven here is that the backend carries them at
   * all, on the platform where they are Winsock and not syscalls. */
  {
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    push(&d, IORING_OP_SOCKET, AF_INET, NULL, IPPROTO_TCP, SOCK_STREAM, 0x301);
    e = slipstream_engine_enter(d.fd, 1, 1, IORING_ENTER_GETEVENTS, NULL, 0);
    struct io_uring_cqe *sc = cq_pop(&d);
    const int lfd = sc != NULL ? sc->res : -1;
    check(e == 1 && lfd >= 0, "socket answers a descriptor");

    struct io_uring_sqe *bs = push(&d, IORING_OP_BIND, lfd, &a, 0, 0, 0x302);
    bs->off = sizeof(a);
    e = slipstream_engine_enter(d.fd, 1, 1, IORING_ENTER_GETEVENTS, NULL, 0);
    sc = cq_pop(&d);
    check(e == 1 && sc != NULL && sc->res == 0, "bind takes the address");

    push(&d, IORING_OP_LISTEN, lfd, NULL, 1, 0, 0x303);
    e = slipstream_engine_enter(d.fd, 1, 1, IORING_ENTER_GETEVENTS, NULL, 0);
    sc = cq_pop(&d);
    check(e == 1 && sc != NULL && sc->res == 0, "listen answers 0");

    /* accept and connect, against each other on the same ring. Both are
     * issued, so the ring holds two ops and both come back. */
    socklen_t alen = (socklen_t) sizeof(a);
    check(getsockname((SOCKET_T) lfd, (struct sockaddr *) &a, (void *) &alen) == 0,
          "the listener says which port it took");

    push(&d, IORING_OP_ACCEPT, lfd, NULL, 0, 0, 0x305);
    e = slipstream_engine_enter(d.fd, 1, 0, 0, NULL, 0);
    check(e == 1 && cq_ready(&d) == 0, "accept is issued and waits");

    push(&d, IORING_OP_SOCKET, AF_INET, NULL, IPPROTO_TCP, SOCK_STREAM, 0x306);
    e = slipstream_engine_enter(d.fd, 1, 1, IORING_ENTER_GETEVENTS, NULL, 0);
    sc = cq_pop(&d);
    const int cfd = sc != NULL ? sc->res : -1;
    check(cfd >= 0, "a client socket");

    struct io_uring_sqe *cs = push(&d, IORING_OP_CONNECT, cfd, &a, 0, 0, 0x307);
    cs->off = sizeof(a);
    e = slipstream_engine_enter(d.fd, 1, 2, IORING_ENTER_GETEVENTS, NULL, 0);
    int got_accept = -1;
    int got_connect = 1;
    for (int k = 0; k < 2; k++) {
      struct io_uring_cqe *q = cq_pop(&d);
      if (q == NULL) break;
      if (q->user_data == 0x305) got_accept = q->res;
      if (q->user_data == 0x307) got_connect = q->res;
    }
    check(got_connect == 0, "connect answers 0");
    check(got_accept >= 0, "accept answers the new socket");

    /* And the accepted socket is a socket: it carries the peer. */
    struct sockaddr_in peer;
    socklen_t plen = (socklen_t) sizeof(peer);
    check(got_accept >= 0 &&
              getpeername((SOCKET_T) got_accept, (struct sockaddr *) &peer, (void *) &plen) == 0,
          "and it knows its peer");

    if (got_accept >= 0) sock_close(got_accept);
    sock_close(cfd);

    push(&d, IORING_OP_CLOSE, lfd, NULL, 0, 0, 0x304);
    e = slipstream_engine_enter(d.fd, 1, 1, IORING_ENTER_GETEVENTS, NULL, 0);
    sc = cq_pop(&d);
    check(e == 1 && sc != NULL && sc->res == 0, "and close gives it back");
  }

  /* 2c: poll_add says READABLE and takes nothing off the socket. */
  {
    int pp[2] = { -1, -1 };
    check(pair_of_streams(pp) == 0, "a socketpair for the poll");
    push(&d, IORING_OP_POLL_ADD, pp[0], NULL, 0, 0, 0x401)->poll32_events = POLLIN;
    e = slipstream_engine_enter(d.fd, 1, 0, 0, NULL, 0);
    check(cq_ready(&d) == 0, "poll waits while nothing is readable");
    check(peer_write(pp[1], "p", 1) == 1, "the peer writes one byte");
    e = slipstream_engine_enter(d.fd, 0, 1, IORING_ENTER_GETEVENTS, NULL, 0);
    struct io_uring_cqe *pc = cq_pop(&d);
    check(pc != NULL && pc->user_data == 0x401 && (pc->res & POLLIN) != 0,
          "and answers POLLIN when it is");
    char peek[4] = { 0 };
    check(peer_read(pp[0], peek, sizeof(peek)) == 1 && peek[0] == 'p',
          "the byte is still there - poll took nothing");
    sock_close(pp[0]);
    sock_close(pp[1]);
  }

  /* 2d: a file, through the ring. openat, write at an offset, read it
   * back, and the bytes have to be the ones written - a res that agrees
   * over a file that does not would be no proof at all. */
  {
    static const char tmpname[] = "slip_backend_file.tmp";
    static char wbuf[] = "engine";
    static char rbuf[8];
    push(&d, IORING_OP_OPENAT, AT_FDCWD, (void *) tmpname, 0666, 0, 0x501)->open_flags =
        O_RDWR | O_CREAT | O_TRUNC;
    e = slipstream_engine_enter(d.fd, 1, 1, IORING_ENTER_GETEVENTS, NULL, 0);
    struct io_uring_cqe *fc = cq_pop(&d);
    const int ffd = fc != NULL ? fc->res : -1;
    check(ffd >= 0, "openat makes a file");

    push(&d, IORING_OP_WRITE, ffd, wbuf, 6, 0, 0x502);
    e = slipstream_engine_enter(d.fd, 1, 1, IORING_ENTER_GETEVENTS, NULL, 0);
    fc = cq_pop(&d);
    check(fc != NULL && fc->res == 6, "write puts six bytes at offset 0");

    memset(rbuf, 0, sizeof(rbuf));
    push(&d, IORING_OP_READ, ffd, rbuf, 6, 0, 0x503);
    e = slipstream_engine_enter(d.fd, 1, 1, IORING_ENTER_GETEVENTS, NULL, 0);
    fc = cq_pop(&d);
    check(fc != NULL && fc->res == 6, "read takes them back");
    check(memcmp(rbuf, "engine", 6) == 0, "and they are the bytes that went in");

    push(&d, IORING_OP_CLOSE, ffd, NULL, 0, 0, 0x504);
    e = slipstream_engine_enter(d.fd, 1, 1, IORING_ENTER_GETEVENTS, NULL, 0);
    fc = cq_pop(&d);
    check(fc != NULL && fc->res == 0, "and the file closes");

    /* statx sees the six bytes, and unlinkat takes the file away. */
    static struct statx stx;
    memset(&stx, 0, sizeof(stx));
    struct io_uring_sqe *xs =
        push(&d, IORING_OP_STATX, AT_FDCWD, (void *) tmpname, 0, 0, 0x505);
    xs->off = (__u64) (uintptr_t) &stx;
    e = slipstream_engine_enter(d.fd, 1, 1, IORING_ENTER_GETEVENTS, NULL, 0);
    fc = cq_pop(&d);
    check(fc != NULL && fc->res == 0, "statx answers 0");
    check(stx.stx_size == 6, "and reports the size that was written");

    push(&d, IORING_OP_UNLINKAT, AT_FDCWD, (void *) tmpname, 0, 0, 0x506);
    e = slipstream_engine_enter(d.fd, 1, 1, IORING_ENTER_GETEVENTS, NULL, 0);
    fc = cq_pop(&d);
    check(fc != NULL && fc->res == 0, "unlinkat removes it");

    memset(&stx, 0, sizeof(stx));
    xs = push(&d, IORING_OP_STATX, AT_FDCWD, (void *) tmpname, 0, 0, 0x507);
    xs->off = (__u64) (uintptr_t) &stx;
    e = slipstream_engine_enter(d.fd, 1, 1, IORING_ENTER_GETEVENTS, NULL, 0);
    fc = cq_pop(&d);
    check(fc != NULL && fc->res < 0, "and statx says it is gone");
    remove(tmpname);
  }

  /* 3: an idle wait with a deadline says -ETIME, after the deadline. */
  struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 200 * 1000000LL };
  struct io_uring_getevents_arg arg = { .ts = (__u64) (uintptr_t) &ts };
  const long long t0 = ms_now();
  e = slipstream_engine_enter(d.fd, 0, 1, IORING_ENTER_GETEVENTS | IORING_ENTER_EXT_ARG,
                              &arg, sizeof(arg));
  const long long waited = ms_now() - t0;
  check(e == -ETIME, "an idle wait with a deadline answers -ETIME");
  /* 180 and not 190: GetTickCount64 advances in ~15.6ms steps, so a
   * true 200ms can measure as little as ~185. */
  check(waited >= 180, "and not before the deadline");

  /* 4: what the ENGINE can and this host's io_uring may not. The
   * socket commands are young, and a kernel that predates
   * SOCKET_URING_OP_GETSOCKNAME answers nothing at all - while the
   * engine reaches it with getsockname(), which every host has had for
   * decades. So it is not measured against io_uring here; it is
   * measured against the plain call on the same descriptor, which is
   * the only answer that means anything either way. */
#ifndef _WIN32
  {
    struct sockaddr_storage want;
    socklen_t want_len = sizeof(want);
    check(getsockname(sp[0], (struct sockaddr *) &want, &want_len) == 0,
          "the plain getsockname answers on this socket");

    struct sockaddr_storage got;
    memset(&got, 0, sizeof(got));
    socklen_t got_len = sizeof(got);
    /* The fields io_uring_prep_cmd_getsockname writes, written by hand
     * because this test drives the engine without liburing. */
    struct io_uring_sqe *sq = push(&d, IORING_OP_URING_CMD, sp[0], NULL, 0, 0, 0x404);
    sq->cmd_op = SOCKET_URING_OP_GETSOCKNAME;
    sq->addr = (__u64) (uintptr_t) &got;
    sq->addr3 = (__u64) (uintptr_t) &got_len;
    sq->optlen = 0; /* this socket's own name; 1 would ask for the peer's */
    e = slipstream_engine_enter(d.fd, 1, 1, IORING_ENTER_GETEVENTS, NULL, 0);
    struct io_uring_cqe *gc = cq_pop(&d);
    check(e == 1 && gc != NULL && gc->res == 0, "getsockname through the ring answers 0");
    check(got_len == want_len && memcmp(&got, &want, want_len) == 0,
          "and hands back the same address the plain call does");
  }
#endif

  /* 5: the parked set is bounded by the OP POOL, which is what the
   * caller sized the ring to - not by a constant. A ring asked for more
   * holds more, and nothing is ever refused with -EBUSY, which no
   * io_uring answers to anything. They park on ONE descriptor on
   * purpose: it is the parked SET being measured, not how many files
   * the platform can watch. */
  {
    struct drv big;
    static char sink[1500][4];
    const unsigned many = (unsigned) (sizeof(sink) / sizeof(sink[0]));
    if (ring_open_n(&big, 2048) == 0) {
      int bp[2] = { -1, -1 };
      check(pair_of_streams(bp) == 0, "a socketpair for the parked set");
      for (unsigned i = 0; i < many; i++)
        push(&big, IORING_OP_RECV, bp[0], sink[i], sizeof(sink[0]), 0, 0x500 + i);
      e = slipstream_engine_enter(big.fd, many, 0, 0, NULL, 0);
      check(e == (int) many, "1500 recvs on one socket are all submitted");
      int refused = 0;
      while (cq_ready(&big) > 0)
        if (cq_pop(&big)->res == -EBUSY) refused = 1;
      check(!refused, "and none of them is refused - the 1024 ceiling is gone");
      check(peer_write(bp[1], "x", 1) == 1, "the peer writes one byte");
      e = slipstream_engine_enter(big.fd, 0, 1, IORING_ENTER_GETEVENTS, NULL, 0);
      struct io_uring_cqe *bc = cq_pop(&big);
      check(e == 0 && bc != NULL && bc->res == 1, "which completes one of them");
      sock_close(bp[0]);
      sock_close(bp[1]);
      check(slipstream_engine_close(big.fd) == 0, "and that ring goes too");
    }
  }

  sock_close(sp[0]);
  sock_close(sp[1]);
  check(slipstream_engine_close(d.fd) == 0, "the ring goes when it is closed");
}

int main(void) {
#ifdef _WIN32
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
#endif
  scenes("select");
  scenes("epoll");
  scenes("kqueue");
  scenes("dispatch");
  scenes("iocp");

  check(slipstream_engine_backend_set("iocp-of-the-moon") == -EINVAL,
        "an unknown backend is refused by name");

  printf(failed ? "backends: FAILED\n" : "backends: ok\n");
  return failed;
}
