/* What liburing needs from underneath when there is no kernel to ask.
 *
 * liburing does three things after __sys_io_uring_setup: it maps the two
 * rings and the SQE array at the offsets it was handed, it fills its own
 * struct io_uring from the offsets we report, and from then on its
 * inlines read and write that memory. We own __sys_mmap too, so none of
 * it has to be a file - the "fd" from setup is a token, and a map of it
 * at one of the three ring offsets hands back the block we allocated.
 *
 * THE ONE RULE OF ENTER: it never hangs on a single op. The kernel
 * completes plenty of work inline during io_uring_enter - a read out of
 * the page cache, a send with room in the buffer - and so does this
 * engine. What the kernel never does is stall the whole ring behind one
 * recv on an empty socket, and neither does this: an op that would block
 * is parked with the events it waits for, the engine thread polls the
 * parked set, and the completion is posted when the descriptor is ready.
 * The multiplexer SlipstreamIO has always been - engine thread, control
 * pipe, poll over the waiting set, a worker only for regular files,
 * which have no readiness to poll for - reading kernel-format SQEs
 * behind liburing instead of its own.
 *
 * What the engine reads is one shape, not fifty: io_uring_prep_rw fills
 * opcode, fd, off, addr and len, and 56 of liburing's prep functions go
 * through it. Each op adds at most one field of its own afterwards.
 */

/* pread/pwrite are POSIX names a bare -std=c11 hides. This is a .c of
 * our own, not a header a consumer includes, so it may say what it needs
 * on its own first line. */
#define _DEFAULT_SOURCE 1

#include "slipstream_engine.h"

#include <errno.h>
#include <linux/io_uring.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <threads.h>
#include <time.h>
#include <unistd.h>

#define SLIP_RINGS_MAX 64
#define SLIP_WAITING_MAX 1024

/* The layout we report, and therefore the one liburing reads through. */
struct sq_ring {
  unsigned head, tail, ring_mask, ring_entries, flags, dropped;
  /* array[] follows */
};
struct cq_ring {
  unsigned head, tail, ring_mask, ring_entries, overflow, flags;
  /* cqes[] follow */
};

/* One submitted op, copied out of the caller's SQE slot so the slot is
 * reusable the moment enter returns - the same promise the kernel makes. */
struct eng_op {
  struct io_uring_sqe sqe;
  struct eng_op *next;
  short wait_events; /* POLLIN/POLLOUT while parked */
  int stalls_queue;  /* a linked file op: the queue waits for the worker */
};

struct slip_ring {
  int in_use;
  unsigned sq_entries, cq_entries;
  void *sq_block;
  void *cq_block;
  struct io_uring_sqe *sqes;
  size_t sq_size, cq_size, sqes_size;

  /* ---- the handoff, and the completions ---------------------------- */
  mtx_t mtx;  /* inbox, worker queue, CQ posting, backlog, blocked_done */
  cnd_t cv;   /* a completion was posted - enter's wait side */
  cnd_t wq_cv;
  struct eng_op *inbox_head, *inbox_tail; /* enter -> engine */
  struct eng_op *backlog_head, *backlog_tail; /* completions the CQ had no room for */
  struct eng_op *wq_head, *wq_tail; /* engine -> worker (regular files) */
  struct eng_op *blocked_done; /* the op the queue stalled on, finished off-thread */
  int blocked_failed; /* that op's result was negative - the chain must know */

  /* ---- what the ENGINE THREAD owns --------------------------------- */
  struct eng_op *queue_head, *queue_tail; /* submitted, in order */
  struct eng_op *waiting[SLIP_WAITING_MAX];
  unsigned waiting_n;
  int chain_failed; /* a linked op failed: cancel the rest of its chain */
  struct eng_op *blocking; /* a linked op is parked or at the worker; the queue waits */

  int ctl_r, ctl_w; /* the poke: "look again" - submissions, shutdown, worker done */
  thrd_t engine;
  int engine_live;
  thrd_t worker;
  int worker_live;
  int stopping;
};

static struct slip_ring g_rings[SLIP_RINGS_MAX];

static struct slip_ring *ring_of(int fd) {
  if ((fd & SLIP_RING_TOKEN) == 0) return NULL;
  const int i = fd & ~SLIP_RING_TOKEN;
  if (i < 0 || i >= SLIP_RINGS_MAX || !g_rings[i].in_use) return NULL;
  return &g_rings[i];
}

static struct sq_ring *sq_of(struct slip_ring *r) { return (struct sq_ring *) r->sq_block; }
static struct cq_ring *cq_of(struct slip_ring *r) { return (struct cq_ring *) r->cq_block; }
static unsigned *sq_array(struct slip_ring *r) {
  return (unsigned *) ((char *) r->sq_block + sizeof(struct sq_ring));
}
static struct io_uring_cqe *cq_cqes(struct slip_ring *r) {
  return (struct io_uring_cqe *) ((char *) r->cq_block + sizeof(struct cq_ring));
}

static unsigned round_up_pow2(unsigned v) {
  unsigned n = 1;
  while (n < v) n <<= 1;
  return n;
}

static void poke(struct slip_ring *r) {
  const char b = 1;
  /* A full pipe already holds a wakeup; a failed write is not an error. */
  (void) !write(r->ctl_w, &b, 1);
}

/* ---- completions ------------------------------------------------------
 * Only two threads post - the engine and the worker - both under mtx.
 * The caller's side is liburing's inlines: they read ktail with acquire
 * and move khead on their own, which is why the tail store is release
 * and why free CQ space is recomputed instead of tracked. */

static unsigned cq_avail(struct slip_ring *r) {
  struct cq_ring *cq = cq_of(r);
  return __atomic_load_n(&cq->tail, __ATOMIC_ACQUIRE) -
         __atomic_load_n(&cq->head, __ATOMIC_ACQUIRE);
}

static int cq_room(struct slip_ring *r) { return cq_avail(r) < r->cq_entries; }

static void post_locked(struct slip_ring *r, __u64 user_data, int res, unsigned flags) {
  struct cq_ring *cq = cq_of(r);
  struct io_uring_cqe *c = &cq_cqes(r)[cq->tail & cq->ring_mask];
  c->user_data = user_data;
  c->res = res;
  c->flags = flags;
  __atomic_store_n(&cq->tail, cq->tail + 1, __ATOMIC_RELEASE);
  cnd_broadcast(&r->cv);
}

/* The caller moves khead without telling us, so room can appear at any
 * time; the backlog is drained wherever the lock is already held. */
static void drain_backlog_locked(struct slip_ring *r) {
  while (r->backlog_head != NULL && cq_room(r)) {
    struct eng_op *op = r->backlog_head;
    r->backlog_head = op->next;
    if (r->backlog_head == NULL) r->backlog_tail = NULL;
    post_locked(r, op->sqe.user_data, (int) op->sqe.__pad2[0], 0);
    free(op);
  }
}

static void post(struct slip_ring *r, struct eng_op *op, int res) {
  mtx_lock(&r->mtx);
  drain_backlog_locked(r);
  if (cq_room(r)) {
    post_locked(r, op->sqe.user_data, res, 0);
    free(op);
  } else {
    /* Parked as a finished result; __pad2[0] is OUR copy of the SQE, not
     * ring memory the caller sees, so the result may ride there. */
    op->sqe.__pad2[0] = (__u64) (unsigned) res;
    op->next = NULL;
    if (r->backlog_tail) r->backlog_tail->next = op;
    else r->backlog_head = op;
    r->backlog_tail = op;
    cq_of(r)->overflow++;
  }
  mtx_unlock(&r->mtx);
}

/* ---- running one op ---------------------------------------------------
 * Three answers: done (post it), would-block (park it with these
 * events), or file (a regular file has no readiness to poll for - the
 * worker runs it). */

enum verdict { RAN, PARK, FILE_OP };

/* liburing spells "wherever the descriptor stands" as an offset of -1,
 * and that is the only spelling answered with read/write rather than
 * pread/pwrite - a socket or a pipe has no offset. */
static int off_is_current(__u64 off) { return off == (__u64) -1; }

static int fd_is_pollable(int fd) {
  struct stat st;
  if (fstat(fd, &st) != 0) return 1; /* let the real call report the error */
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
    case IORING_OP_READ:
      if (!fd_is_pollable(s->fd)) return FILE_OP;
      if (!ready_now(s->fd, POLLIN)) {
        op->wait_events = POLLIN;
        return PARK;
      }
      n = off_is_current(s->off) ? read(s->fd, buf, s->len)
                                 : pread(s->fd, buf, s->len, (off_t) s->off);
      *res_out = n < 0 ? -errno : (int) n;
      return RAN;
    case IORING_OP_WRITE:
      if (!fd_is_pollable(s->fd)) return FILE_OP;
      if (!ready_now(s->fd, POLLOUT)) {
        op->wait_events = POLLOUT;
        return PARK;
      }
      n = off_is_current(s->off) ? write(s->fd, buf, s->len)
                                 : pwrite(s->fd, buf, s->len, (off_t) s->off);
      *res_out = n < 0 ? -errno : (int) n;
      return RAN;
    default:
      /* Known op, not carried here. -EOPNOTSUPP and not -EINVAL: the
       * first says "that op, not here", which is what a caller needs in
       * order to take another route. */
      *res_out = -EOPNOTSUPP;
      return RAN;
  }
}

/* A regular file read or write, run to the end. Blocking is the point:
 * this thread exists so that the engine's poll loop never does. */
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
    post(r, op, res); /* frees op */
    if (stalls) poke(r); /* the queue is waiting on this */

    mtx_lock(&r->mtx);
  }
  mtx_unlock(&r->mtx);
  return 0;
}

static void worker_start_once(struct slip_ring *r) {
  if (r->worker_live) return;
  if (thrd_create(&r->worker, worker_main, r) == thrd_success) r->worker_live = 1;
}

/* ---- the engine thread ------------------------------------------------ */

/* 1 if the op now waits; 0 if the set is full - the caller then answers
 * -EBUSY itself, because only the caller knows the op's chain. */
static int park(struct slip_ring *r, struct eng_op *op) {
  if (r->waiting_n < SLIP_WAITING_MAX) {
    r->waiting[r->waiting_n++] = op;
    return 1;
  }
  return 0;
}

static void process_queue(struct slip_ring *r) {
  while (r->queue_head != NULL && r->blocking == NULL) {
    struct eng_op *op = r->queue_head;
    r->queue_head = op->next;
    if (r->queue_head == NULL) r->queue_tail = NULL;
    op->next = NULL;

    const int linked = (op->sqe.flags & IOSQE_IO_LINK) != 0;

    if (r->chain_failed) {
      /* A failing IOSQE_IO_LINK member cancels the rest of its chain -
       * io_uring_enter(2), IOSQE_IO_LINK. */
      post(r, op, -ECANCELED);
      if (!linked) r->chain_failed = 0;
      continue;
    }

    int res = 0;
    switch (run_one(op, &res)) {
      case RAN:
        if (res < 0 && linked) r->chain_failed = 1;
        post(r, op, res);
        break;
      case PARK:
        if (park(r, op)) {
          if (linked) r->blocking = op; /* the chain waits where it is */
        } else {
          if (linked) r->chain_failed = 1;
          post(r, op, -EBUSY); /* a full waiting set is refused, not dropped */
        }
        break;
      case FILE_OP:
        if (linked) {
          op->stalls_queue = 1;
          r->blocking = op;
        }
        mtx_lock(&r->mtx);
        if (r->wq_tail) r->wq_tail->next = op;
        else r->wq_head = op;
        r->wq_tail = op;
        worker_start_once(r);
        cnd_signal(&r->wq_cv);
        mtx_unlock(&r->mtx);
        break;
    }
  }
}

static int engine_main(void *arg) {
  struct slip_ring *r = arg;
  struct pollfd pfds[SLIP_WAITING_MAX + 1];

  for (;;) {
    pfds[0].fd = r->ctl_r;
    pfds[0].events = POLLIN;
    for (unsigned i = 0; i < r->waiting_n; i++) {
      pfds[i + 1].fd = r->waiting[i]->sqe.fd;
      pfds[i + 1].events = r->waiting[i]->wait_events;
    }
    if (poll(pfds, r->waiting_n + 1, -1) < 0) {
      if (errno == EINTR) continue;
      break;
    }

    if (pfds[0].revents & POLLIN) {
      char sink[64];
      (void) !read(r->ctl_r, sink, sizeof(sink));
      mtx_lock(&r->mtx);
      if (r->stopping) {
        mtx_unlock(&r->mtx);
        break;
      }
      /* New submissions in, and the ticket for a chain the worker
       * finished off-thread. */
      if (r->inbox_head != NULL) {
        if (r->queue_tail) r->queue_tail->next = r->inbox_head;
        else r->queue_head = r->inbox_head;
        r->queue_tail = r->inbox_tail;
        r->inbox_head = r->inbox_tail = NULL;
      }
      if (r->blocked_done != NULL && r->blocked_done == r->blocking) {
        /* The pointer is a ticket - the op behind it is already posted
         * and freed, so it is compared, never read. */
        r->blocking = NULL;
        r->blocked_done = NULL;
        if (r->blocked_failed) {
          r->chain_failed = 1;
          r->blocked_failed = 0;
        }
      }
      drain_backlog_locked(r);
      mtx_unlock(&r->mtx);
    }

    /* Parked descriptors that came ready: run them where they stand -
     * the try is DONTWAIT/poll-guarded, so a spurious wakeup re-parks. */
    unsigned kept = 0;
    for (unsigned i = 0; i < r->waiting_n; i++) {
      struct eng_op *op = r->waiting[i];
      if (!(pfds[i + 1].revents & (op->wait_events | POLLERR | POLLHUP))) {
        r->waiting[kept++] = op;
        continue;
      }
      int res = 0;
      enum verdict v = run_one(op, &res);
      if (v == FILE_OP) { /* a parked op never becomes a file op */
        v = RAN;
        res = -EOPNOTSUPP;
      }
      switch (v) {
        case RAN:
          if (op == r->blocking) {
            r->blocking = NULL;
            if (res < 0) r->chain_failed = 1;
          }
          post(r, op, res);
          break;
        default:
          r->waiting[kept++] = op;
          break;
      }
    }
    r->waiting_n = kept;

    process_queue(r);
  }
  return 0;
}

/* ---- the exported five ------------------------------------------------ */

int slipstream_engine_setup(unsigned int entries, struct io_uring_params *p) {
  if (entries == 0 || p == NULL) return -EINVAL;
  /* Shapes that change what liburing's inlines expect of this memory.
   * Refused by name rather than half-served.
   *
   * IORING_SETUP_NO_SQARRAY is in the list for a different reason, and
   * the -EINVAL is load-bearing: io_uring_queue_init_try_nosqarr asks
   * for it FIRST every time, and falls back to the classic layout only
   * when setup answers exactly -EINVAL. That fallback exists for kernels
   * that do not know the flag, and this is one of them. */
  if (p->flags & (IORING_SETUP_SQPOLL | IORING_SETUP_SQE128 | IORING_SETUP_CQE32 |
                  IORING_SETUP_IOPOLL | IORING_SETUP_NO_MMAP | IORING_SETUP_NO_SQARRAY))
    return -EINVAL;

  int idx = -1;
  for (int i = 0; i < SLIP_RINGS_MAX; i++) {
    if (!g_rings[i].in_use) { idx = i; break; }
  }
  if (idx < 0) return -EMFILE;

  struct slip_ring *r = &g_rings[idx];
  memset(r, 0, sizeof(*r));
  r->sq_entries = round_up_pow2(entries);
  r->cq_entries = p->cq_entries ? round_up_pow2(p->cq_entries) : r->sq_entries * 2;

  r->sq_size = sizeof(struct sq_ring) + (size_t) r->sq_entries * sizeof(unsigned);
  r->cq_size = sizeof(struct cq_ring) + (size_t) r->cq_entries * sizeof(struct io_uring_cqe);
  r->sqes_size = (size_t) r->sq_entries * sizeof(struct io_uring_sqe);

  r->sq_block = calloc(1, r->sq_size);
  r->cq_block = calloc(1, r->cq_size);
  r->sqes = calloc(r->sq_entries, sizeof(struct io_uring_sqe));
  int fds[2] = { -1, -1 };
  if (!r->sq_block || !r->cq_block || !r->sqes || pipe(fds) != 0 ||
      mtx_init(&r->mtx, mtx_plain) != thrd_success ||
      cnd_init(&r->cv) != thrd_success || cnd_init(&r->wq_cv) != thrd_success) {
    free(r->sq_block); free(r->cq_block); free(r->sqes);
    if (fds[0] >= 0) { close(fds[0]); close(fds[1]); }
    memset(r, 0, sizeof(*r));
    return -ENOMEM;
  }
  r->ctl_r = fds[0];
  r->ctl_w = fds[1];

  sq_of(r)->ring_mask = r->sq_entries - 1;
  sq_of(r)->ring_entries = r->sq_entries;
  cq_of(r)->ring_mask = r->cq_entries - 1;
  cq_of(r)->ring_entries = r->cq_entries;

  if (thrd_create(&r->engine, engine_main, r) != thrd_success) {
    close(r->ctl_r); close(r->ctl_w);
    free(r->sq_block); free(r->cq_block); free(r->sqes);
    memset(r, 0, sizeof(*r));
    return -ENOMEM;
  }
  r->engine_live = 1;

  p->sq_entries = r->sq_entries;
  p->cq_entries = r->cq_entries;
  /* EXT_ARG, so liburing's submit_and_wait_timeout hands the timeout to
   * enter instead of prepping an IORING_OP_TIMEOUT this engine does not
   * carry. */
  p->features = IORING_FEAT_EXT_ARG;
  p->sq_off.head = offsetof(struct sq_ring, head);
  p->sq_off.tail = offsetof(struct sq_ring, tail);
  p->sq_off.ring_mask = offsetof(struct sq_ring, ring_mask);
  p->sq_off.ring_entries = offsetof(struct sq_ring, ring_entries);
  p->sq_off.flags = offsetof(struct sq_ring, flags);
  p->sq_off.dropped = offsetof(struct sq_ring, dropped);
  p->sq_off.array = sizeof(struct sq_ring);
  p->cq_off.head = offsetof(struct cq_ring, head);
  p->cq_off.tail = offsetof(struct cq_ring, tail);
  p->cq_off.ring_mask = offsetof(struct cq_ring, ring_mask);
  p->cq_off.ring_entries = offsetof(struct cq_ring, ring_entries);
  p->cq_off.overflow = offsetof(struct cq_ring, overflow);
  p->cq_off.cqes = sizeof(struct cq_ring);
  p->cq_off.flags = offsetof(struct cq_ring, flags);

  r->in_use = 1;
  return idx | SLIP_RING_TOKEN;
}

void *slipstream_engine_mmap(size_t length, int fd, long long offset) {
  struct slip_ring *r = ring_of(fd);
  if (r == NULL) return NULL;
  switch ((unsigned long long) offset) {
    case IORING_OFF_SQ_RING: return length <= r->sq_size ? r->sq_block : NULL;
    case IORING_OFF_CQ_RING: return length <= r->cq_size ? r->cq_block : NULL;
    case IORING_OFF_SQES:    return length <= r->sqes_size ? r->sqes : NULL;
    default: return NULL;
  }
}

/* 0 for a block that is one of a ring's three, -ENOENT for anything
 * else - the caller then knows the mapping was never ours. The blocks
 * themselves go with the ring, in close. */
int slipstream_engine_munmap(void *addr, size_t length) {
  (void) length;
  for (int i = 0; i < SLIP_RINGS_MAX; i++) {
    const struct slip_ring *r = &g_rings[i];
    if (!r->in_use) continue;
    if (addr == r->sq_block || addr == r->cq_block || addr == (void *) r->sqes) return 0;
  }
  return -ENOENT;
}

static void free_op_list(struct eng_op *op) {
  while (op != NULL) {
    struct eng_op *next = op->next;
    free(op);
    op = next;
  }
}

int slipstream_engine_close(int fd) {
  struct slip_ring *r = ring_of(fd);
  if (r == NULL) return -EBADF;

  mtx_lock(&r->mtx);
  r->stopping = 1;
  cnd_broadcast(&r->wq_cv);
  cnd_broadcast(&r->cv);
  mtx_unlock(&r->mtx);
  poke(r);
  if (r->engine_live) thrd_join(r->engine, NULL);
  if (r->worker_live) thrd_join(r->worker, NULL);

  free_op_list(r->inbox_head);
  free_op_list(r->backlog_head);
  free_op_list(r->wq_head);
  free_op_list(r->queue_head);
  for (unsigned i = 0; i < r->waiting_n; i++) free(r->waiting[i]);
  close(r->ctl_r);
  close(r->ctl_w);
  mtx_destroy(&r->mtx);
  cnd_destroy(&r->cv);
  cnd_destroy(&r->wq_cv);
  free(r->sq_block);
  free(r->cq_block);
  free(r->sqes);
  memset(r, 0, sizeof(*r));
  return 0;
}

/* IORING_ENTER_EXT_ARG: the timeout rides beside the call instead of
 * being an op. Relative, like the kernel takes it. */
static int deadline_of(unsigned int flags, const void *arg, size_t sz,
                       struct timespec *out, int *has_deadline) {
  *has_deadline = 0;
  if (!(flags & IORING_ENTER_EXT_ARG)) return 0;
  if (arg == NULL || sz != sizeof(struct io_uring_getevents_arg)) return -EINVAL;
  const struct io_uring_getevents_arg *a = arg;
  const struct __kernel_timespec *ts = (const struct __kernel_timespec *) (uintptr_t) a->ts;
  if (ts == NULL) return 0;
  timespec_get(out, TIME_UTC);
  out->tv_sec += (time_t) ts->tv_sec;
  out->tv_nsec += (long) ts->tv_nsec;
  if (out->tv_nsec >= 1000000000L) {
    out->tv_sec += 1;
    out->tv_nsec -= 1000000000L;
  }
  *has_deadline = 1;
  return 0;
}

int slipstream_engine_enter(int fd, unsigned int to_submit, unsigned int min_complete,
                            unsigned int flags, const void *arg, size_t argsz) {
  struct slip_ring *r = ring_of(fd);
  if (r == NULL) return -EBADF;

  struct timespec deadline;
  int has_deadline = 0;
  const int drc = deadline_of(flags, arg, argsz, &deadline, &has_deadline);
  if (drc != 0) return drc;

  /* Take the submitted SQEs off the ring - copied, so the slots are the
   * caller's again the moment this returns, which is the promise the
   * kernel makes too. */
  struct sq_ring *sq = sq_of(r);
  unsigned submitted = 0;
  struct eng_op *batch_head = NULL, *batch_tail = NULL;
  const unsigned tail = __atomic_load_n(&sq->tail, __ATOMIC_ACQUIRE);
  while (sq->head != tail && submitted < to_submit) {
    const unsigned i = sq_array(r)[sq->head & sq->ring_mask];
    if (i < r->sq_entries) {
      struct eng_op *op = calloc(1, sizeof(*op));
      if (op == NULL) break;
      op->sqe = r->sqes[i];
      if (batch_tail) batch_tail->next = op;
      else batch_head = op;
      batch_tail = op;
    } else {
      sq->dropped++; /* an index past the array: dropped, like the kernel counts it */
    }
    __atomic_store_n(&sq->head, sq->head + 1, __ATOMIC_RELEASE);
    submitted++;
  }
  if (batch_head != NULL) {
    mtx_lock(&r->mtx);
    if (r->inbox_tail) r->inbox_tail->next = batch_head;
    else r->inbox_head = batch_head;
    r->inbox_tail = batch_tail;
    mtx_unlock(&r->mtx);
    poke(r);
  }

  if ((flags & IORING_ENTER_GETEVENTS) && min_complete > 0) {
    struct cq_ring *cq = cq_of(r);
    mtx_lock(&r->mtx);
    for (;;) {
      drain_backlog_locked(r);
      const unsigned avail = __atomic_load_n(&cq->tail, __ATOMIC_ACQUIRE) -
                             __atomic_load_n(&cq->head, __ATOMIC_ACQUIRE);
      if (avail >= min_complete) break;
      if (r->stopping) {
        mtx_unlock(&r->mtx);
        return -EBADF;
      }
      if (has_deadline) {
        if (cnd_timedwait(&r->cv, &r->mtx, &deadline) == thrd_timedout) {
          mtx_unlock(&r->mtx);
          return -ETIME;
        }
      } else {
        cnd_wait(&r->cv, &r->mtx);
      }
    }
    mtx_unlock(&r->mtx);
  }

  return (int) submitted;
}
