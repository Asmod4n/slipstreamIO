/* The macOS backend, completion-shaped: dispatch_io DOES what io_uring
 * does - it runs the read or write itself and reports the outcome -
 * where a dispatch_source would only have said "ready". So this sits in
 * the same family as iocp: execute issues, wait translates results,
 * nothing parks. kqueue stays ruled out on macOS (operational
 * experience); GCD is the surface Apple keeps working. Proven against
 * swift-corelibs-libdispatch - GCD's own code - on Linux, which is also
 * why this file needs clang: dispatch_io speaks blocks.
 *
 * What dispatch_io cannot say, the worker runs: recv/send with flags
 * (MSG_PEEK and friends have no dispatch spelling) block a worker
 * thread and stay correct. Plain recv/send are stream reads and writes,
 * which they are on a socket.
 *
 * Two contracts hold the sharp edges:
 *   - io_uring completes a socket read SHORT, with what arrived first;
 *     dispatch_io would happily wait for the full length, so the low
 *     water mark is 1 and the FIRST delivery finishes the op.
 *   - close with ops in flight must not free what a handler may still
 *     touch: every channel is stopped (DISPATCH_IO_STOP fires the
 *     handlers with ECANCELED), and close drains until the in-flight
 *     list is empty. */
#if defined(__APPLE__) || defined(SLIPSTREAM_HAVE_LIBDISPATCH)

#include "engine_internal.h"

#include <dispatch/dispatch.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct dsp_state {
  dispatch_queue_t q;
  dispatch_semaphore_t sem;
  mtx_t mtx; /* done[], in-flight list, the finished flags */
  struct eng_done done[SLIP_WAITING_MAX];
  unsigned done_n;
  struct dsp_io *flying;
};

/* One issued channel operation. Lives until its final handler ran AND
 * the engine collected it. */
struct dsp_io {
  struct slip_ring *r;
  struct eng_op *op;
  dispatch_io_t ch;
  struct dsp_io *next;
  size_t copied;
  int finished;
};

static void dsp_finish_locked(struct dsp_io *io, int res) {
  struct dsp_state *st = io->r->be_state;
  if (io->finished) return;
  io->finished = 1;
  if (st->done_n < SLIP_WAITING_MAX) {
    st->done[st->done_n].op = io->op;
    st->done[st->done_n].res = res;
    st->done_n++;
  }
  struct dsp_io **p = &st->flying;
  while (*p != NULL && *p != io) p = &(*p)->next;
  if (*p == io) *p = io->next;
}

static void dsp_finish(struct dsp_io *io, int res) {
  struct dsp_state *st = io->r->be_state;
  mtx_lock(&st->mtx);
  dsp_finish_locked(io, res);
  mtx_unlock(&st->mtx);
  dispatch_semaphore_signal(st->sem);
}

static void dsp_noop(void *arg) { (void) arg; }

static int dsp_open_ring(struct slip_ring *r) {
  struct dsp_state *st = calloc(1, sizeof(*st));
  if (st == NULL) return -1;
  if (mtx_init(&st->mtx, mtx_plain) != thrd_success) {
    free(st);
    return -1;
  }
  st->q = dispatch_queue_create("slipstreamio.engine", DISPATCH_QUEUE_SERIAL);
  st->sem = dispatch_semaphore_create(0);
  r->be_state = st;
  return 0;
}

static void dsp_close_ring(struct slip_ring *r) {
  struct dsp_state *st = r->be_state;
  if (st == NULL) return;
  /* Stop every channel still flying; each fires its handler with
   * ECANCELED and lands in done[]. The ops are freed here - the ring is
   * going, nobody collects them. */
  mtx_lock(&st->mtx);
  for (struct dsp_io *io = st->flying; io != NULL; io = io->next)
    dispatch_io_close(io->ch, DISPATCH_IO_STOP);
  mtx_unlock(&st->mtx);
  for (;;) {
    mtx_lock(&st->mtx);
    const int flying = st->flying != NULL;
    /* The op is ours to free; the wrapper is the final handler's. */
    for (unsigned i = 0; i < st->done_n; i++) free(st->done[i].op);
    st->done_n = 0;
    mtx_unlock(&st->mtx);
    if (!flying) break;
    dispatch_semaphore_wait(st->sem, DISPATCH_TIME_FOREVER);
  }
  dispatch_sync_f(st->q, NULL, dsp_noop); /* every in-flight handler done */
  dispatch_release(st->q);
  dispatch_release(st->sem);
  mtx_destroy(&st->mtx);
  free(st);
  r->be_state = NULL;
}

static void dsp_poke(struct slip_ring *r) {
  struct dsp_state *st = r->be_state;
  dispatch_semaphore_signal(st->sem);
}

static struct dsp_io *dsp_io_new(struct slip_ring *r, struct eng_op *op, int fd, __u64 off) {
  struct dsp_state *st = r->be_state;
  struct dsp_io *io = calloc(1, sizeof(*io));
  if (io == NULL) return NULL;
  io->r = r;
  io->op = op;
  const int random = off != (__u64) -1;
  io->ch = dispatch_io_create(random ? DISPATCH_IO_RANDOM : DISPATCH_IO_STREAM, fd, st->q,
                              ^(int error) { (void) error; });
  if (io->ch == NULL) {
    free(io);
    return NULL;
  }
  dispatch_io_set_low_water(io->ch, 1);
  mtx_lock(&st->mtx);
  io->next = st->flying;
  st->flying = io;
  mtx_unlock(&st->mtx);
  op->be_source = io;
  return io;
}

static int dsp_execute(struct slip_ring *r, struct eng_op *op, int *res) {
  const struct io_uring_sqe *s = &op->sqe;
  char *buf = (char *) (uintptr_t) s->addr;
  switch (s->opcode) {
    case IORING_OP_NOP:
      *res = 0;
      return EXEC_DONE;
    case IORING_OP_CLOSE:
      *res = close(s->fd) == 0 ? 0 : -errno;
      return EXEC_DONE;
    case IORING_OP_URING_CMD:
      /* A socket command is a syscall that answers at once - dispatch
       * has no channel to open for it. */
      *res = slip_posix_cmd_sock(s);
      return EXEC_DONE;
    case IORING_OP_RECV:
    case IORING_OP_SEND:
      if (s->msg_flags != 0) {
        /* No dispatch spelling for recv flags - the worker runs the
         * real call and blocks where blocking belongs. */
        slip_posix_hand_to_worker(r, op);
        return EXEC_PENDING;
      }
      /* flags 0 is a stream read or write, which is what it is - the
       * same issue path as READ/WRITE below. */
    /* FALLTHROUGH */
    case IORING_OP_READ:
    case IORING_OP_WRITE: {
      const int writing = s->opcode == IORING_OP_WRITE || s->opcode == IORING_OP_SEND;
      const __u64 off = (s->opcode == IORING_OP_RECV || s->opcode == IORING_OP_SEND)
                            ? (__u64) -1
                            : s->off;
      struct dsp_io *io = dsp_io_new(r, op, s->fd, off);
      if (io == NULL) {
        *res = -ENOMEM;
        return EXEC_DONE;
      }
      const off_t o = off == (__u64) -1 ? 0 : (off_t) off;
      const unsigned len = s->len;
      /* Lifecycle, exactly once: the handler invocation with done==true
       * is dispatch_io's LAST word on an operation - only there may the
       * channel be released and the wrapper freed. Releasing inside an
       * earlier invocation trapped SIGILL in _os_object_release,
       * measured: deliveries were still queued behind it. */
      if (writing) {
        dispatch_data_t d = dispatch_data_create(buf, len, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        dispatch_io_write(io->ch, o, d, ((struct dsp_state *) r->be_state)->q,
                          ^(bool done, dispatch_data_t remaining, int error) {
                            if (!done) return;
                            if (!io->finished) {
                              const size_t left =
                                  remaining ? dispatch_data_get_size(remaining) : 0;
                              dsp_finish(io, error ? -error : (int) (len - left));
                            }
                            dispatch_release(io->ch);
                            free(io);
                          });
        dispatch_release(d);
      } else {
        dispatch_io_read(io->ch, o, len, ((struct dsp_state *) r->be_state)->q,
                         ^(bool done, dispatch_data_t data, int error) {
                           const size_t got = data ? dispatch_data_get_size(data) : 0;
                           if (!io->finished && got > 0 && io->copied < len) {
                             dispatch_data_apply(
                                 data, ^bool(dispatch_data_t rg, size_t o2, const void *p, size_t sz) {
                                   (void) rg;
                                   size_t room = len - io->copied;
                                   if (o2 >= room) return true;
                                   if (sz > room - o2) sz = room - o2;
                                   memcpy(buf + io->copied + o2, p, sz);
                                   return true;
                                 });
                             io->copied += got > len - io->copied ? len - io->copied : got;
                           }
                           /* io_uring completes short: the first bytes
                            * finish the op; done with nothing is EOF or
                            * the error. */
                           if (!io->finished && (io->copied > 0 || done)) {
                             dsp_finish(io, error && io->copied == 0 ? -error : (int) io->copied);
                             if (!done) dispatch_io_close(io->ch, DISPATCH_IO_STOP);
                           }
                           if (done) {
                             dispatch_release(io->ch);
                             free(io);
                           }
                         });
      }
      return EXEC_PENDING;
    }
    default:
      *res = -EOPNOTSUPP;
      return EXEC_DONE;
  }
}

static int dsp_wait_done(struct slip_ring *r, struct eng_done *out, unsigned max,
                         int timeout_ms) {
  struct dsp_state *st = r->be_state;
  const dispatch_time_t until =
      timeout_ms < 0 ? DISPATCH_TIME_FOREVER
                     : dispatch_time(DISPATCH_TIME_NOW, (int64_t) timeout_ms * NSEC_PER_MSEC);
  if (dispatch_semaphore_wait(st->sem, until) != 0) return 0; /* the deadline, not an event */
  mtx_lock(&st->mtx);
  unsigned n = 0;
  while (st->done_n > 0 && n < max) {
    st->done_n--;
    out[n].op = st->done[st->done_n].op;
    out[n].res = st->done[st->done_n].res;
    /* The wrapper stays with its final handler; the op leaves here and
     * must not point at it any more. */
    out[n].op->be_source = NULL;
    n++;
  }
  mtx_unlock(&st->mtx);
  return (int) n; /* zero is a bare poke */
}

static const unsigned char dsp_carried_ops[] = {
  IORING_OP_NOP, IORING_OP_READ, IORING_OP_WRITE, IORING_OP_RECV,
  IORING_OP_SEND, IORING_OP_CLOSE, IORING_OP_URING_CMD, 255,
};

const struct eng_backend slip_backend_dispatch = {
  .name = "dispatch",
  .open_ring = dsp_open_ring,
  .close_ring = dsp_close_ring,
  .poke = dsp_poke,
  .execute = dsp_execute,
  .wait = dsp_wait_done,
  .carried_ops = dsp_carried_ops,
  .arm = NULL, /* completion family: nothing parks */
  .disarm = NULL,
};

#else
typedef int slip_backend_dispatch_needs_libdispatch;
#endif
