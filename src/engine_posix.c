/* The one implementation behind every readiness backend. poll, epoll,
 * kqueue and dispatch differ only in how they learn that a parked
 * descriptor came ready; how an op is TRIED, parked, retried, or sent
 * to the worker - and how the engine is poked - is the same machine,
 * and it lives here once.
 *
 * pread/pwrite are POSIX names a bare -std=c11 hides; a .c of our own
 * may say what it needs on its first line. */
#ifndef _WIN32
#define _DEFAULT_SOURCE 1

#include "engine_internal.h"

#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- the poke pipe ---------------------------------------------------- */

int slip_posix_ctl_open(struct slip_ring *r) {
  int fds[2];
  if (pipe(fds) != 0) return -1;
  r->ctl_r = fds[0];
  r->ctl_w = fds[1];
  /* Drained until empty on every wakeup; it must never block. */
  fcntl(r->ctl_r, F_SETFL, O_NONBLOCK);
  return 0;
}

void slip_posix_ctl_close(struct slip_ring *r) {
  if (r->ctl_r >= 0) close(r->ctl_r);
  if (r->ctl_w >= 0) close(r->ctl_w);
  r->ctl_r = r->ctl_w = -1;
}

void slip_posix_ctl_drain(struct slip_ring *r) {
  char sink[64];
  while (read(r->ctl_r, sink, sizeof(sink)) > 0) { }
}

void slip_posix_poke(struct slip_ring *r) {
  const char b = 1;
  /* A full pipe already holds a wakeup; a failed write is not an error. */
  (void) !write(r->ctl_w, &b, 1);
}

/* ---- running one op ---------------------------------------------------
 * Three answers: done, would-block (park it with these events), or file
 * (a regular file has no readiness to poll for - the worker runs it). */

enum verdict { RAN, PARK, FILE_OP };

/* liburing spells "wherever the descriptor stands" as an offset of -1,
 * and that is the only spelling answered with read/write rather than
 * pread/pwrite - a socket or a pipe has no offset. */
static int off_is_current(__u64 off) { return off == (__u64) -1; }

/* 1 readiness, 0 regular file, -1 the descriptor itself is broken - and
 * then the REAL call runs right away and reports it, because parking a
 * bad descriptor waits forever: poll ignores negative fds, so a parked
 * read on fd -1 would never fire, where the kernel answers -EBADF. */
static int fd_is_pollable(int fd) {
  struct stat st;
  if (fstat(fd, &st) != 0) return -1;
  return !(S_ISREG(st.st_mode) || S_ISBLK(st.st_mode) || S_ISDIR(st.st_mode));
}

static int ready_now(int fd, short events) {
  struct pollfd p = { .fd = fd, .events = events };
  return poll(&p, 1, 0) > 0;
}

static enum verdict run_one(struct eng_op *op, int *res_out) {
  const struct io_uring_sqe *s = &op->sqe;
  void *buf = (void *) (uintptr_t) s->addr;
  ssize_t n;
  switch (s->opcode) {
    case IORING_OP_NOP:
      *res_out = 0;
      return RAN;
    case IORING_OP_CLOSE:
      n = close(s->fd);
      *res_out = n < 0 ? -errno : 0;
      return RAN;
    case IORING_OP_RECV:
      n = recv(s->fd, buf, s->len, (int) s->msg_flags | MSG_DONTWAIT);
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
          !(s->msg_flags & MSG_DONTWAIT)) {
        op->wait_events = POLLIN;
        return PARK;
      }
      *res_out = n < 0 ? -errno : (int) n;
      return RAN;
    case IORING_OP_SEND:
      n = send(s->fd, buf, s->len, (int) s->msg_flags | MSG_DONTWAIT);
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
          !(s->msg_flags & MSG_DONTWAIT)) {
        op->wait_events = POLLOUT;
        return PARK;
      }
      *res_out = n < 0 ? -errno : (int) n;
      return RAN;
    case IORING_OP_READ: {
      const int pollable = fd_is_pollable(s->fd);
      if (pollable == 0) return FILE_OP;
      if (pollable > 0 && !ready_now(s->fd, POLLIN)) {
        op->wait_events = POLLIN;
        return PARK;
      }
      n = off_is_current(s->off) ? read(s->fd, buf, s->len)
                                 : pread(s->fd, buf, s->len, (off_t) s->off);
      *res_out = n < 0 ? -errno : (int) n;
      return RAN;
    }
    case IORING_OP_WRITE: {
      const int pollable = fd_is_pollable(s->fd);
      if (pollable == 0) return FILE_OP;
      if (pollable > 0 && !ready_now(s->fd, POLLOUT)) {
        op->wait_events = POLLOUT;
        return PARK;
      }
      n = off_is_current(s->off) ? write(s->fd, buf, s->len)
                                 : pwrite(s->fd, buf, s->len, (off_t) s->off);
      *res_out = n < 0 ? -errno : (int) n;
      return RAN;
    }
    default:
      /* Known op, not carried here. -EOPNOTSUPP and not -EINVAL: the
       * first says "that op, not here", which is what a caller needs in
       * order to take another route. */
      *res_out = -EOPNOTSUPP;
      return RAN;
  }
}

/* ---- the worker: regular files, run to the end ------------------------
 * Blocking is the point: this thread exists so that the engine's wait
 * loop never does. */

static int run_file_op(const struct io_uring_sqe *s) {
  void *buf = (void *) (uintptr_t) s->addr;
  ssize_t n;
  switch (s->opcode) {
    case IORING_OP_READ:
      n = off_is_current(s->off) ? read(s->fd, buf, s->len)
                                 : pread(s->fd, buf, s->len, (off_t) s->off);
      break;
    case IORING_OP_WRITE:
      n = off_is_current(s->off) ? write(s->fd, buf, s->len)
                                 : pwrite(s->fd, buf, s->len, (off_t) s->off);
      break;
    case IORING_OP_RECV:
      n = recv(s->fd, buf, s->len, (int) s->msg_flags);
      break;
    case IORING_OP_SEND:
      n = send(s->fd, buf, s->len, (int) s->msg_flags);
      break;
    default:
      return -EOPNOTSUPP;
  }
  return n < 0 ? -errno : (int) n;
}

static int worker_main(void *arg) {
  struct slip_ring *r = arg;
  mtx_lock(&r->mtx);
  for (;;) {
    while (r->wq_head == NULL && !r->stopping) cnd_wait(&r->wq_cv, &r->mtx);
    if (r->stopping) break;
    struct eng_op *op = r->wq_head;
    r->wq_head = op->next;
    if (r->wq_head == NULL) r->wq_tail = NULL;
    mtx_unlock(&r->mtx);

    const int res = run_file_op(&op->sqe);
    const int stalls = op->stalls_queue;
    if (stalls) {
      /* The ticket goes up before post frees the op. The engine only
       * compares the pointer against r->blocking, never reads through
       * it. */
      mtx_lock(&r->mtx);
      r->blocked_done = op;
      r->blocked_failed = res < 0;
      mtx_unlock(&r->mtx);
    }
    slip_engine_post(r, op, res); /* frees op */
    if (stalls) slip_posix_poke(r); /* the queue is waiting on this */

    mtx_lock(&r->mtx);
  }
  mtx_unlock(&r->mtx);
  return 0;
}

static void worker_start_once(struct slip_ring *r) {
  if (r->worker_live) return;
  if (thrd_create(&r->worker, worker_main, r) == thrd_success) r->worker_live = 1;
}

/* ---- parking ---------------------------------------------------------- */

/* 1 if the op now waits with the backend armed; 0 if the set is full or
 * the backend refused. */
static int park(struct slip_ring *r, struct eng_op *op) {
  if (r->waiting_n >= SLIP_WAITING_MAX) return 0;
  if (r->be->arm(r, op) != 0) return 0;
  r->waiting[r->waiting_n++] = op;
  return 1;
}

/* Removal first, then disarm: a backend that merges per-descriptor
 * interest recomputes it from waiting[], which must no longer hold the
 * leaving op. */
static void unpark(struct slip_ring *r, struct eng_op *op) {
  for (unsigned i = 0; i < r->waiting_n; i++) {
    if (r->waiting[i] == op) {
      r->waiting[i] = r->waiting[--r->waiting_n];
      r->be->disarm(r, op);
      return;
    }
  }
}

void slip_posix_hand_to_worker(struct slip_ring *r, struct eng_op *op) {
  /* stalls_queue rides on the op BEFORE the worker can see it - the
   * ticket compare in the core needs it set by then. */
  if (op->sqe.flags & IOSQE_IO_LINK) op->stalls_queue = 1;
  mtx_lock(&r->mtx);
  if (r->wq_tail) r->wq_tail->next = op;
  else r->wq_head = op;
  r->wq_tail = op;
  worker_start_once(r);
  cnd_signal(&r->wq_cv);
  mtx_unlock(&r->mtx);
}

int slip_posix_execute(struct slip_ring *r, struct eng_op *op, int *res) {
  switch (run_one(op, res)) {
    case RAN:
      return EXEC_DONE;
    case PARK:
      if (park(r, op)) return EXEC_PENDING;
      *res = -EBUSY; /* a full waiting set is refused, not dropped */
      return EXEC_DONE;
    case FILE_OP:
      slip_posix_hand_to_worker(r, op);
      return EXEC_PENDING;
  }
  *res = -EINVAL;
  return EXEC_DONE;
}

int slip_posix_finish_ready(struct slip_ring *r, struct eng_op **ready, unsigned ready_n,
                            struct eng_done *out, unsigned max) {
  unsigned n = 0;
  for (unsigned i = 0; i < ready_n && n < max; i++) {
    struct eng_op *op = ready[i];
    unpark(r, op);
    int res = 0;
    enum verdict v = run_one(op, &res);
    if (v == FILE_OP) { /* a parked op never becomes a file op */
      v = RAN;
      res = -EOPNOTSUPP;
    }
    if (v == RAN) {
      out[n].op = op;
      out[n].res = res;
      n++;
    } else if (!park(r, op)) { /* spurious wakeup, and now the set is full */
      out[n].op = op;
      out[n].res = -EBUSY;
      n++;
    }
  }
  return (int) n;
}

#else
typedef int slip_engine_posix_is_not_windows;
#endif
