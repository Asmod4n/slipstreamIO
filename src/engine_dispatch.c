/* The macOS backend, spelled in GCD - and it is a READINESS backend,
 * because that is what io_uring is: try the op, and only what answers
 * EAGAIN waits for the descriptor to say it is ready. A dispatch source
 * says exactly that, so the source's handler does NOT run anything - it
 * notes which descriptor woke and signals the semaphore, and the op
 * itself runs on the submitter's thread through the SAME shared
 * machinery every other backend uses.
 *
 * What has no readiness to wait for - a positioned read on a regular
 * file, a connect, an openat - goes to GCD, never to a thread of ours:
 *   - a positioned file read or write becomes a dispatch_io channel,
 *     which is what Apple offers for exactly that, and it completes
 *     when the whole request is done, the way read(2) on a regular file
 *     does;
 *   - everything else runs the plain blocking call on the global
 *     concurrent queue - GCD's own pool, which is where io_uring puts
 *     the same ops.
 * Both come back through done[] and are handed out by wait on the
 * submitter's thread, so the core settles chains the ordinary way.
 *
 * Sources are made when a descriptor's side gains its first waiter and
 * cancelled when it loses its last. A fresh registration is the one
 * that is guaranteed to report readiness that is ALREADY there; a
 * source kept across an idle period is not, and this engine only ever
 * parks after a real EAGAIN, so nothing is lost by paying for it.
 *
 * Proven against swift-corelibs-libdispatch - GCD's own code - on
 * Linux, which is also why this file needs clang: dispatch speaks
 * blocks. */
#if defined(__APPLE__) || defined(SLIPSTREAM_HAVE_LIBDISPATCH)

#include "engine_internal.h"

#include <dispatch/dispatch.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Per descriptor: the two sources, and who waits behind each. A second
 * op on the same side chains through the op's own next pointer, which a
 * parked op is not otherwise using - the same table the kqueue backend
 * keeps, for the same reason: a wakeup names its own waiters instead of
 * being searched for. */
struct dsp_fd {
  dispatch_source_t rd, wr;
  struct eng_op *in, *out;
  int rd_hit, wr_hit; /* a handler reported readiness, not yet answered */
  int queued;         /* on the ready chain */
  int next_ready;     /* index of the next descriptor on it, -1 ends */
};

struct dsp_state {
  dispatch_queue_t q;       /* serial: every handler runs here */
  dispatch_queue_t blocking; /* Apple's pool, for what must block */
  /* The wait's door. Its count is 1 exactly while `knocked` says
   * something is waiting to be collected - signalled on the TRANSITION,
   * never per item. A counting semaphore would hand out one surplus
   * wakeup per event already answered, and a wakeup with nothing behind
   * it is not free here: enter has one pass, so it turns a caller's
   * deadline into -ETIME on the spot. Measured - the deadline scene
   * answered -ETIME immediately instead of at the deadline. */
  dispatch_semaphore_t sem;
  int knocked;
  /* A second door, for close_ring only: a source that has finished
   * cancelling and an op that has come back off a queue are nobody's
   * completion, and must not wake a wait. */
  dispatch_semaphore_t gate;
  mtx_t mtx; /* fds[], the ready chain, done[], live, knocked */

  struct dsp_fd *fds;
  unsigned fds_n;
  int ready_head;

  /* op_pool_n long, like the ring's own arrays: no more ops can be out
   * on a GCD queue at once than the pool holds. */
  struct eng_done *done;
  unsigned done_n, done_max;

  /* Sources not yet finished cancelling, plus ops still out on a queue.
   * close_ring waits for this to reach zero: a handler that ran after
   * the state was freed would be reading a dangling pointer. */
  unsigned live;
};

/* One dispatch_io request in flight. Lives until its final handler ran. */
struct dsp_io {
  struct slip_ring *r;
  struct eng_op *op;
  dispatch_io_t ch;
  size_t copied;
};

static struct dsp_fd *dsp_slot(struct slip_ring *r, int fd) {
  struct dsp_state *st = r->be_state;
  if (fd < 0) return NULL;
  if ((unsigned) fd >= st->fds_n) {
    unsigned want = st->fds_n ? st->fds_n : 64;
    while (want <= (unsigned) fd) want *= 2;
    /* Under the lock: a source handler reads this array. */
    mtx_lock(&st->mtx);
    struct dsp_fd *grown = realloc(st->fds, (size_t) want * sizeof(*grown));
    if (grown == NULL) {
      mtx_unlock(&st->mtx);
      return NULL;
    }
    memset(grown + st->fds_n, 0, (size_t) (want - st->fds_n) * sizeof(*grown));
    for (unsigned i = st->fds_n; i < want; i++) grown[i].next_ready = -1;
    st->fds = grown;
    st->fds_n = want;
    mtx_unlock(&st->mtx);
  }
  return &st->fds[fd];
}

/* Called with the lock held: 1 if the caller owes the door a knock. */
static int dsp_knock_locked(struct dsp_state *st) {
  if (st->knocked) return 0;
  st->knocked = 1;
  return 1;
}

/* The handler's whole job: note the side, and knock. Nothing about the
 * op is touched here - the op runs on the submitter's thread. */
static void dsp_report(struct slip_ring *r, int fd, int writing) {
  struct dsp_state *st = r->be_state;
  int knock = 0;
  mtx_lock(&st->mtx);
  if ((unsigned) fd < st->fds_n) {
    struct dsp_fd *slot = &st->fds[fd];
    if (writing) slot->wr_hit = 1;
    else slot->rd_hit = 1;
    if (!slot->queued) {
      slot->queued = 1;
      slot->next_ready = st->ready_head;
      st->ready_head = fd;
    }
    knock = dsp_knock_locked(st);
  }
  mtx_unlock(&st->mtx);
  if (knock) dispatch_semaphore_signal(st->sem);
}

static void dsp_source_stop(struct slip_ring *r, dispatch_source_t src) {
  (void) r;
  if (src == NULL) return;
  dispatch_source_cancel(src); /* live drops in the cancel handler */
  dispatch_release(src);
}

static dispatch_source_t dsp_source_start(struct slip_ring *r, int fd, int writing) {
  struct dsp_state *st = r->be_state;
  dispatch_source_t src = dispatch_source_create(
      writing ? DISPATCH_SOURCE_TYPE_WRITE : DISPATCH_SOURCE_TYPE_READ, (uintptr_t) fd, 0,
      st->q);
  if (src == NULL) return NULL;
  mtx_lock(&st->mtx);
  st->live++;
  mtx_unlock(&st->mtx);
  dispatch_source_set_event_handler(src, ^{ dsp_report(r, fd, writing); });
  dispatch_source_set_cancel_handler(src, ^{
    mtx_lock(&st->mtx);
    st->live--;
    mtx_unlock(&st->mtx);
    dispatch_semaphore_signal(st->gate); /* close_ring's door, not the wait's */
  });
  dispatch_resume(src);
  return src;
}

static void dsp_noop(void *arg) { (void) arg; }

static int dsp_open_ring(struct slip_ring *r) {
  struct dsp_state *st = calloc(1, sizeof(*st));
  if (st == NULL) return -1;
  if (mtx_init(&st->mtx, mtx_plain) != thrd_success) {
    free(st);
    return -1;
  }
  st->done = calloc(r->op_pool_n, sizeof(*st->done));
  if (st->done == NULL) {
    mtx_destroy(&st->mtx);
    free(st);
    return -1;
  }
  st->q = dispatch_queue_create("slipstreamio.engine", DISPATCH_QUEUE_SERIAL);
  st->blocking = dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0);
  st->done_max = r->op_pool_n;
  st->sem = dispatch_semaphore_create(0);
  st->gate = dispatch_semaphore_create(0);
  st->ready_head = -1;
  r->be_state = st;
  return 0;
}

static void dsp_close_ring(struct slip_ring *r) {
  struct dsp_state *st = r->be_state;
  if (st == NULL) return;
  /* Every source goes, and every op still out on a queue is waited for:
   * a handler firing after this state is freed reads a dangling
   * pointer, and there is no way to recall an op GCD is already
   * running. */
  for (unsigned i = 0; i < st->fds_n; i++) {
    dsp_source_stop(r, st->fds[i].rd);
    dsp_source_stop(r, st->fds[i].wr);
    st->fds[i].rd = st->fds[i].wr = NULL;
  }
  for (;;) {
    mtx_lock(&st->mtx);
    /* The ops are DROPPED, not freed. An eng_op is a slot in the ring's
     * op_pool, which is one allocation carved up at setup and released
     * whole with r->block - free() on a pointer into the middle of it
     * is what glibc calls an invalid pointer, and it aborts. Nothing
     * collects these because the ring is going; letting go is the whole
     * of it. */
    st->done_n = 0;
    const unsigned live = st->live;
    mtx_unlock(&st->mtx);
    if (live == 0) break;
    dispatch_semaphore_wait(st->gate, DISPATCH_TIME_FOREVER);
  }
  dispatch_sync_f(st->q, NULL, dsp_noop); /* every queued handler done */
  dispatch_release(st->q);
  dispatch_release(st->sem);
  dispatch_release(st->gate);
  mtx_destroy(&st->mtx);
  free(st->fds);
  free(st->done);
  free(st);
  r->be_state = NULL;
}

static void dsp_poke(struct slip_ring *r) {
  struct dsp_state *st = r->be_state;
  mtx_lock(&st->mtx);
  const int knock = dsp_knock_locked(st);
  mtx_unlock(&st->mtx);
  if (knock) dispatch_semaphore_signal(st->sem);
}

/* ---- parking: the source is the registration ------------------------- */

static int dsp_arm(struct slip_ring *r, struct eng_op *op) {
  struct dsp_fd *slot = dsp_slot(r, op->sqe.fd);
  if (slot == NULL) return -1;
  /* POLLPRI has no source of its own, so it rides the read side - the
   * retry then reads whatever actually came, exactly as on kqueue. */
  const int writing = (op->wait_events & POLLOUT) != 0;
  struct eng_op **side = writing ? &slot->out : &slot->in;
  dispatch_source_t *src = writing ? &slot->wr : &slot->rd;
  const int first = *side == NULL;
  op->next = *side;
  *side = op;
  if (!first) return 0; /* the source is there, and someone is behind it */
  *src = dsp_source_start(r, op->sqe.fd, writing);
  if (*src != NULL) return 0;
  *side = op->next;
  op->next = NULL;
  return -1;
}

/* The last waiter leaving takes the source with it. Keeping it would
 * save a registration and cost the guarantee that matters: only a fresh
 * source is certain to report readiness that arrived while nobody was
 * listening. */
static void dsp_disarm(struct slip_ring *r, struct eng_op *op) {
  struct dsp_fd *slot = dsp_slot(r, op->sqe.fd);
  if (slot == NULL) return;
  const int writing = (op->wait_events & POLLOUT) != 0;
  struct eng_op **side = writing ? &slot->out : &slot->in;
  dispatch_source_t *src = writing ? &slot->wr : &slot->rd;
  for (struct eng_op **p = side; *p != NULL; p = &(*p)->next) {
    if (*p == op) {
      *p = op->next;
      op->next = NULL;
      break;
    }
  }
  if (*side == NULL && *src != NULL) {
    dsp_source_stop(r, *src);
    *src = NULL;
  }
}

/* The descriptor is closing. A source outliving its descriptor is a
 * source on a number that comes back as something else, so it goes
 * here - before the close, which is where the core calls this. */
static void dsp_forget(struct slip_ring *r, int fd) {
  struct dsp_state *st = r->be_state;
  if (fd < 0 || (unsigned) fd >= st->fds_n) return;
  struct dsp_fd *slot = &st->fds[fd];
  dsp_source_stop(r, slot->rd);
  dsp_source_stop(r, slot->wr);
  slot->rd = slot->wr = NULL;
  slot->in = slot->out = NULL;
  mtx_lock(&st->mtx);
  slot->rd_hit = slot->wr_hit = 0;
  mtx_unlock(&st->mtx);
}

/* ---- what has no readiness: GCD runs it ------------------------------ */

static void dsp_done(struct slip_ring *r, struct eng_op *op, int res) {
  struct dsp_state *st = r->be_state;
  mtx_lock(&st->mtx);
  if (st->done_n < st->done_max) {
    st->done[st->done_n].op = op;
    st->done[st->done_n].res = res;
    st->done_n++;
  }
  st->live--;
  const int knock = dsp_knock_locked(st);
  mtx_unlock(&st->mtx);
  if (knock) dispatch_semaphore_signal(st->sem);
  dispatch_semaphore_signal(st->gate); /* close_ring counts this one too */
}

/* A positioned read or write on a regular file: dispatch_io is what
 * Apple offers for exactly this. It completes when the request is
 * DONE - a regular file has no "some of it arrived first" the way a
 * socket does, and read(2) on one returns everything up to EOF. */
static int dsp_file_channel(struct slip_ring *r, struct eng_op *op) {
  struct dsp_state *st = r->be_state;
  const struct io_uring_sqe *s = &op->sqe;
  char *buf = (char *) (uintptr_t) s->addr;
  const unsigned len = s->len;
  const off_t off = (off_t) s->off;
  const int writing = s->opcode == IORING_OP_WRITE;

  struct dsp_io *io = calloc(1, sizeof(*io));
  if (io == NULL) return -1;
  io->r = r;
  io->op = op;
  io->ch = dispatch_io_create(DISPATCH_IO_RANDOM, s->fd, st->q, ^(int error) { (void) error; });
  if (io->ch == NULL) {
    free(io);
    return -1;
  }
  mtx_lock(&st->mtx);
  st->live++;
  mtx_unlock(&st->mtx);

  /* Lifecycle, exactly once: the handler invocation with done==true is
   * dispatch_io's LAST word on a request - only there may the channel be
   * released and the wrapper freed. Releasing inside an earlier
   * invocation trapped SIGILL in _os_object_release, measured:
   * deliveries were still queued behind it. */
  if (writing) {
    dispatch_data_t d = dispatch_data_create(buf, len, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    dispatch_io_write(io->ch, off, d, st->q, ^(bool done, dispatch_data_t remaining, int error) {
      if (!done) return;
      const size_t left = remaining ? dispatch_data_get_size(remaining) : 0;
      dsp_done(io->r, io->op, error && left == len ? -error : (int) (len - left));
      dispatch_io_close(io->ch, 0);
      dispatch_release(io->ch);
      free(io);
    });
    dispatch_release(d);
  } else {
    dispatch_io_read(io->ch, off, len, st->q, ^(bool done, dispatch_data_t data, int error) {
      if (data != NULL && dispatch_data_get_size(data) > 0 && io->copied < len) {
        const size_t base = io->copied;
        dispatch_data_apply(data, ^bool(dispatch_data_t rg, size_t o2, const void *p, size_t sz) {
          (void) rg;
          const size_t room = len - base;
          if (o2 >= room) return true;
          if (sz > room - o2) sz = room - o2;
          memcpy(buf + base + o2, p, sz);
          return true;
        });
        const size_t got = dispatch_data_get_size(data);
        io->copied += got > len - base ? len - base : got;
      }
      if (!done) return;
      dsp_done(io->r, io->op, error && io->copied == 0 ? -error : (int) io->copied);
      dispatch_io_close(io->ch, 0);
      dispatch_release(io->ch);
      free(io);
    });
  }
  return 0;
}

static int dsp_is_regular(int fd) {
  struct stat st;
  return fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
}

static void dsp_submit_blocking(struct slip_ring *r, struct eng_op *op) {
  struct dsp_state *st = r->be_state;
  const struct io_uring_sqe *s = &op->sqe;
  /* The channel serves the positioned file read and write. An op that
   * asks for "wherever the descriptor stands" is read(2)'s contract,
   * including the offset it advances, which a channel does not keep -
   * so that one goes to the pool with everything else. */
  if ((s->opcode == IORING_OP_READ || s->opcode == IORING_OP_WRITE) &&
      s->off != (__u64) -1 && dsp_is_regular(s->fd)) {
    if (dsp_file_channel(r, op) == 0) return;
  }
  mtx_lock(&st->mtx);
  st->live++;
  mtx_unlock(&st->mtx);
  dispatch_async(st->blocking, ^{ dsp_done(r, op, slip_posix_run_blocking(&op->sqe)); });
}

/* ---- the wait: GCD's results, and the descriptors that came ready ---- */

static int dsp_wait(struct slip_ring *r, struct eng_done *out, unsigned max, int timeout_ms) {
  struct dsp_state *st = r->be_state;
  const dispatch_time_t until =
      timeout_ms < 0 ? DISPATCH_TIME_FOREVER
                     : dispatch_time(DISPATCH_TIME_NOW, (int64_t) timeout_ms * NSEC_PER_MSEC);
  if (dispatch_semaphore_wait(st->sem, until) != 0) return 0; /* the deadline, not an event */

  struct eng_op **ready = r->ready;
  unsigned n = 0, rn = 0;

  mtx_lock(&st->mtx);
  while (st->done_n > 0 && n < max) {
    st->done_n--;
    out[n].op = st->done[st->done_n].op;
    out[n].res = st->done[st->done_n].res;
    n++;
  }
  /* NO SEARCH: the descriptor that woke hands over its own waiters.
   * A descriptor whose waiters do not fit STAYS on the chain with its
   * hit intact - clearing news nobody answered is how an op waits
   * forever. The chain is walked only while there is room to answer
   * from, so the next wait picks up exactly what is left. */
  int i = (n < max) ? st->ready_head : -1;
  while (i >= 0) {
    struct dsp_fd *slot = &st->fds[i];
    unsigned want = 0;
    if (slot->rd_hit)
      for (struct eng_op *op = slot->in; op != NULL; op = op->next) want++;
    if (slot->wr_hit)
      for (struct eng_op *op = slot->out; op != NULL; op = op->next) want++;
    if (rn + want > r->op_pool_n) break; /* this one waits its turn */
    if (slot->rd_hit) {
      slot->rd_hit = 0;
      for (struct eng_op *op = slot->in; op != NULL; op = op->next) ready[rn++] = op;
    }
    if (slot->wr_hit) {
      slot->wr_hit = 0;
      for (struct eng_op *op = slot->out; op != NULL; op = op->next) ready[rn++] = op;
    }
    i = slot->next_ready;
    slot->next_ready = -1;
    slot->queued = 0;
    st->ready_head = i;
  }
  /* The door closes only when there is nothing left behind it; what is
   * still there keeps its knock, so the next wait comes straight back. */
  const int left = st->ready_head >= 0 || st->done_n > 0;
  if (!left) st->knocked = 0;
  mtx_unlock(&st->mtx);
  if (left) dispatch_semaphore_signal(st->sem);

  if (rn > 0 && n < max)
    n += (unsigned) slip_posix_finish_ready(r, ready, rn, out + n, max - n);
  return (int) n; /* zero is a bare poke */
}

const struct eng_backend slip_backend_dispatch = {
  .name = "dispatch",
  .open_ring = dsp_open_ring,
  .close_ring = dsp_close_ring,
  .poke = dsp_poke,
  .execute = slip_posix_execute,
  .wait = dsp_wait,
  /* The same list every readiness backend answers - one io_uring, not a
   * smaller one because the platform is macOS. */
  .carried_ops = slip_posix_carried_ops,
  .arm = dsp_arm,
  .disarm = dsp_disarm,
  .submit_blocking = dsp_submit_blocking,
  .forget = dsp_forget,
};

#else
typedef int slip_backend_dispatch_needs_libdispatch;
#endif
