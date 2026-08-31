/* The GCD backend: Apple's maintained readiness surface, and the macOS
 * default - kqueue is ruled out there by operational experience, and
 * dispatch sources are what Apple keeps working. Also compiles and runs
 * against swift-corelibs-libdispatch anywhere else, which is how it is
 * proven without a Mac.
 *
 * Why dispatch_source and not dispatch_io: dispatch_io owns the reads
 * and writes itself - a stream of buffers, no recv flags, no
 * MSG_DONTWAIT - so it cannot carry io_uring's op semantics. Sources
 * only say "ready", and the engine's own DONTWAIT/poll-guarded retry
 * stays the one that runs the op, the same as every other backend.
 *
 * Delivery inverts here: handlers fire on a serial queue, the engine
 * thread blocks in wait. A fired list under a lock and a semaphore
 * carry it across. Cancellation is the sharp edge - a handler may be
 * mid-flight on the queue while disarm runs - so disarm cancels, then
 * waits for the queue with dispatch_sync, and only then purges the op
 * from the fired list; after that nothing can deliver it again. */
#if defined(__APPLE__) || defined(SLIPSTREAM_HAVE_LIBDISPATCH)

#include "engine_internal.h"

#include <dispatch/dispatch.h>
#include <poll.h>
#include <stdlib.h>

struct dsp_state {
  dispatch_queue_t q;
  dispatch_semaphore_t sem;
  dispatch_source_t ctl;
  mtx_t fired_mtx;
  struct eng_op *fired[SLIP_WAITING_MAX];
  unsigned fired_n;
};

struct dsp_source_ctx {
  struct slip_ring *r;
  struct eng_op *op;
};

static void dsp_fire(void *arg) {
  struct dsp_source_ctx *c = arg;
  struct dsp_state *st = c->r->be_state;
  mtx_lock(&st->fired_mtx);
  int seen = 0;
  for (unsigned i = 0; i < st->fired_n; i++) {
    if (st->fired[i] == c->op) { seen = 1; break; }
  }
  if (!seen && st->fired_n < SLIP_WAITING_MAX) st->fired[st->fired_n++] = c->op;
  mtx_unlock(&st->fired_mtx);
  dispatch_semaphore_signal(st->sem);
}

static void dsp_ctx_free(void *arg) { free(arg); }

static void dsp_poke_signal(void *arg) {
  struct dsp_state *st = ((struct slip_ring *) arg)->be_state;
  dispatch_semaphore_signal(st->sem);
}

static void dsp_noop(void *arg) { (void) arg; }

static int dispatch_open_ring(struct slip_ring *r) {
  if (slip_posix_ctl_open(r) != 0) return -1;
  struct dsp_state *st = calloc(1, sizeof(*st));
  if (st == NULL) {
    slip_posix_ctl_close(r);
    return -1;
  }
  if (mtx_init(&st->fired_mtx, mtx_plain) != thrd_success) {
    free(st);
    slip_posix_ctl_close(r);
    return -1;
  }
  st->q = dispatch_queue_create("slipstreamio.engine", DISPATCH_QUEUE_SERIAL);
  st->sem = dispatch_semaphore_create(0);
  r->be_state = st;

  st->ctl = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t) r->ctl_r, 0, st->q);
  if (st->ctl == NULL) {
    dispatch_release(st->q);
    dispatch_release(st->sem);
    mtx_destroy(&st->fired_mtx);
    free(st);
    r->be_state = NULL;
    slip_posix_ctl_close(r);
    return -1;
  }
  dispatch_set_context(st->ctl, r);
  dispatch_source_set_event_handler_f(st->ctl, dsp_poke_signal);
  dispatch_resume(st->ctl);
  return 0;
}

static void dispatch_close_ring(struct slip_ring *r) {
  struct dsp_state *st = r->be_state;
  if (st == NULL) return;
  dispatch_source_cancel(st->ctl);
  dispatch_sync_f(st->q, NULL, dsp_noop); /* every in-flight handler done */
  dispatch_release(st->ctl);
  dispatch_release(st->q);
  dispatch_release(st->sem);
  mtx_destroy(&st->fired_mtx);
  free(st);
  r->be_state = NULL;
  slip_posix_ctl_close(r);
}

static int dispatch_arm(struct slip_ring *r, struct eng_op *op) {
  struct dsp_state *st = r->be_state;
  struct dsp_source_ctx *c = malloc(sizeof(*c));
  if (c == NULL) return -1;
  c->r = r;
  c->op = op;
  dispatch_source_t src = dispatch_source_create(
      (op->wait_events & POLLOUT) ? DISPATCH_SOURCE_TYPE_WRITE : DISPATCH_SOURCE_TYPE_READ,
      (uintptr_t) op->sqe.fd, 0, st->q);
  if (src == NULL) {
    free(c);
    return -1;
  }
  dispatch_set_context(src, c);
  dispatch_source_set_event_handler_f(src, dsp_fire);
  dispatch_source_set_cancel_handler_f(src, dsp_ctx_free);
  op->be_source = (void *) src;
  dispatch_resume(src);
  return 0;
}

static void dispatch_disarm(struct slip_ring *r, struct eng_op *op) {
  struct dsp_state *st = r->be_state;
  dispatch_source_t src = (dispatch_source_t) op->be_source;
  if (src == NULL) return;
  dispatch_source_cancel(src);
  dispatch_sync_f(st->q, NULL, dsp_noop); /* handlers and the cancel handler done */
  dispatch_release(src);
  op->be_source = NULL;
  mtx_lock(&st->fired_mtx);
  for (unsigned i = 0; i < st->fired_n; i++) {
    if (st->fired[i] == op) {
      st->fired[i] = st->fired[--st->fired_n];
      break;
    }
  }
  mtx_unlock(&st->fired_mtx);
}

static int dispatch_wait(struct slip_ring *r, struct eng_done *out, unsigned max) {
  struct dsp_state *st = r->be_state;
  dispatch_semaphore_wait(st->sem, DISPATCH_TIME_FOREVER);
  slip_posix_ctl_drain(r);
  struct eng_op *ready[SLIP_WAITING_MAX];
  mtx_lock(&st->fired_mtx);
  unsigned n = 0;
  while (st->fired_n > 0 && n < SLIP_WAITING_MAX) ready[n++] = st->fired[--st->fired_n];
  mtx_unlock(&st->fired_mtx);
  /* zero is a bare poke, or a delivery already drained */
  return slip_posix_finish_ready(r, ready, n, out, max);
}

const struct eng_backend slip_backend_dispatch = {
  .name = "dispatch",
  .open_ring = dispatch_open_ring,
  .close_ring = dispatch_close_ring,
  .poke = slip_posix_poke,
  .execute = slip_posix_execute,
  .wait = dispatch_wait,
  .arm = dispatch_arm,
  .disarm = dispatch_disarm,
};

#else
typedef int slip_backend_dispatch_needs_libdispatch;
#endif
