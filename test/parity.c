/* The behavior, not only the API: the kernel is the oracle. Every
 * scenario here runs twice through the SAME liburing calls - once with
 * the kernel answering, once with the engine forced - and the two
 * completion streams must match field for field: user_data, res, count,
 * and order where io_uring promises order (a link chain). Where it does
 * not, completions are compared as a set.
 *
 * This is what the fixed park-forever bug looked like from outside: a
 * read on fd -1 answered -EBADF by the kernel and never at all by the
 * engine. A scenario below holds that door shut.
 *
 * Needs a kernel that allows io_uring - the oracle - and says so and
 * skips when there is none. Built and run by test/with_liburing.sh. */
#include <liburing.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "slipstream_syscall.h"

static int failed;

struct rec {
  unsigned long long user_data;
  int res;
};

#define REC_MAX 16

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
    io_uring_cqe_seen(ring, cqe);
    n++;
  }
  return n;
}

static int cmp_rec(const void *a, const void *b) {
  const struct rec *x = a, *y = b;
  if (x->user_data != y->user_data) return x->user_data < y->user_data ? -1 : 1;
  return x->res - y->res;
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

static int sc_wait_times_out(struct io_uring *ring, struct rec *out) {
  struct __kernel_timespec ts = { .tv_sec = 0, .tv_nsec = 100 * 1000000LL };
  struct io_uring_cqe *cqe = NULL;
  const int rc = io_uring_wait_cqe_timeout(ring, &cqe, &ts);
  out[0].user_data = 0xE;
  out[0].res = rc;
  return 1;
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
  { "a wait with nothing coming times out", sc_wait_times_out, 1 },
};

static int run_side(const struct scenario *sc, int engine, struct rec *out) {
  slipstream_syscall_set_engine(engine);
  struct io_uring ring;
  if (io_uring_queue_init(8, &ring, 0) != 0) return -1;
  const int n = sc->run(&ring, out);
  io_uring_queue_exit(&ring);
  return n;
}

int main(void) {
  /* The oracle first: without a kernel that answers, there is nothing
   * to compare against. */
  slipstream_syscall_set_engine(0);
  struct io_uring probe;
  if (io_uring_queue_init(2, &probe, 0) != 0) {
    printf("parity: this kernel refuses io_uring - no oracle, skipped\n");
    return 0;
  }
  io_uring_queue_exit(&probe);

  const unsigned count = sizeof(scenarios) / sizeof(scenarios[0]);
  for (unsigned i = 0; i < count; i++) {
    struct rec kernel[REC_MAX], engine[REC_MAX];
    const int nk = run_side(&scenarios[i], 0, kernel);
    const int ne = run_side(&scenarios[i], 1, engine);
    int same = nk >= 0 && nk == ne;
    if (same && !scenarios[i].ordered) {
      qsort(kernel, (size_t) nk, sizeof(struct rec), cmp_rec);
      qsort(engine, (size_t) ne, sizeof(struct rec), cmp_rec);
    }
    for (int j = 0; same && j < nk; j++) {
      same = kernel[j].user_data == engine[j].user_data && kernel[j].res == engine[j].res;
    }
    printf("  %-48s %s\n", scenarios[i].what, same ? "same on both sides" : "DIVERGED");
    if (!same) {
      failed = 1;
      for (int j = 0; j < (nk > ne ? nk : ne); j++) {
        printf("    [%d] kernel", j);
        if (j < nk) printf(" data=%llu res=%d", kernel[j].user_data, kernel[j].res);
        else printf(" (nothing)");
        printf("  engine");
        if (j < ne) printf(" data=%llu res=%d", engine[j].user_data, engine[j].res);
        else printf(" (nothing)");
        printf("\n");
      }
    }
  }

  printf(failed ? "parity: DIVERGED from the kernel\n" : "parity: the engine behaves like the kernel\n");
  return failed;
}
