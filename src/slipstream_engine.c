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
 * recv on an empty socket, and neither does this: an op that cannot
 * finish now goes to the backend - parked for readiness, issued as an
 * overlapped, handed to the worker - and its completion is posted when
 * it comes back. HOW is the whole difference between platforms, and it
 * is all a backend is - see engine_internal.h. This file is everything
 * else: order, chains, the CQ, enter. It compiles on every platform the
 * engine serves, Windows included, and holds not one OS call.
 *
 * What the engine reads is one shape, not fifty: io_uring_prep_rw fills
 * opcode, fd, off, addr and len, and 56 of liburing's prep functions go
 * through it. Each op adds at most one field of its own afterwards.
 */
#include "slipstream_engine.h"
#include "engine_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static struct slip_ring g_rings[SLIP_RINGS_MAX];

/* The backend is a per-process choice, made by the platform and
 * overridable by name until the first ring exists - a switch under
 * running rings would strand their pending ops. */
static const struct eng_backend *g_backend;

static const struct eng_backend *backend_default(void) {
#if defined(_WIN32)
  return &slip_backend_iocp;
#elif defined(__APPLE__)
  return &slip_backend_dispatch; /* kqueue is ruled out there - TASKS.md */
#elif defined(__linux__)
  return &slip_backend_epoll;
#elif defined(__FreeBSD__) || defined(__DragonFly__) || defined(__NetBSD__) || \
    defined(__OpenBSD__)
  return &slip_backend_kqueue;
#else
  return &slip_backend_poll;
#endif
}

static const struct eng_backend *backend_by_name(const char *name) {
#ifndef _WIN32
  if (strcmp(name, slip_backend_poll.name) == 0) return &slip_backend_poll;
#endif
#ifdef __linux__
  if (strcmp(name, slip_backend_epoll.name) == 0) return &slip_backend_epoll;
#endif
#if defined(__FreeBSD__) || defined(__DragonFly__) || defined(__NetBSD__) || \
    defined(__OpenBSD__) || defined(__APPLE__) || defined(SLIPSTREAM_HAVE_LIBKQUEUE)
  if (strcmp(name, slip_backend_kqueue.name) == 0) return &slip_backend_kqueue;
#endif
#if defined(__APPLE__) || defined(SLIPSTREAM_HAVE_LIBDISPATCH)
  if (strcmp(name, slip_backend_dispatch.name) == 0) return &slip_backend_dispatch;
#endif
#ifdef _WIN32
  if (strcmp(name, slip_backend_iocp.name) == 0) return &slip_backend_iocp;
#endif
  return NULL;
}

int slipstream_engine_backend_set(const char *name) {
  const struct eng_backend *be = backend_by_name(name);
  if (be == NULL) return -EINVAL; /* not carried on this platform: said, not ignored */
  for (int i = 0; i < SLIP_RINGS_MAX; i++) {
    if (g_rings[i].in_use) return -EBUSY; /* rings already run on the current one */
  }
  g_backend = be;
  return 0;
}

const char *slipstream_engine_backend_name(void) {
  return (g_backend ? g_backend : backend_default())->name;
}

static struct slip_ring *ring_of(int fd) {
  if ((fd & SLIP_RING_TOKEN) == 0) return NULL;
  const int i = fd & ~SLIP_RING_TOKEN;
  if (i < 0 || i >= SLIP_RINGS_MAX || !g_rings[i].in_use) return NULL;
  return &g_rings[i];
}

/* The layout we report, and therefore the one liburing reads through. */
struct sq_ring {
  unsigned head, tail, ring_mask, ring_entries, flags, dropped;
  /* array[] follows */
};
struct cq_ring {
  unsigned head, tail, ring_mask, ring_entries, overflow, flags;
  /* cqes[] follow */
};

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

/* ---- completions ------------------------------------------------------
 * Whoever finishes an op posts - the engine thread, the worker, iocp's
 * drain - all under mtx. The caller's side is liburing's inlines: they
 * read ktail with acquire and move khead on their own, which is why the
 * tail store is release and why free CQ space is recomputed instead of
 * tracked. */

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
    post_locked(r, op->sqe.user_data, (int) op->sqe.__pad2[0], op->cqe_flags);
    free(op);
  }
}

static void backlog_locked(struct slip_ring *r, struct eng_op *op, int res) {
  /* Parked as a finished result; __pad2[0] is OUR copy of the SQE, not
   * ring memory the caller sees, so the result may ride there. */
  op->sqe.__pad2[0] = (__u64) (unsigned) res;
  op->next = NULL;
  if (r->backlog_tail) r->backlog_tail->next = op;
  else r->backlog_head = op;
  r->backlog_tail = op;
  cq_of(r)->overflow++;
}

void slip_engine_post(struct slip_ring *r, struct eng_op *op, int res) {
  mtx_lock(&r->mtx);
  drain_backlog_locked(r);
  if (cq_room(r)) {
    post_locked(r, op->sqe.user_data, res, op->cqe_flags);
    free(op);
  } else {
    backlog_locked(r, op, res);
  }
  mtx_unlock(&r->mtx);
}

/* A multishot emission: the CQE without the op. When the CQ is full the
 * emission rides the backlog on a carrier op of its own; when even that
 * cannot be had, the overflow counter is the honest remainder. */
void slip_engine_emit(struct slip_ring *r, __u64 user_data, int res, unsigned flags) {
  mtx_lock(&r->mtx);
  drain_backlog_locked(r);
  if (cq_room(r)) {
    post_locked(r, user_data, res, flags);
  } else {
    struct eng_op *carrier = calloc(1, sizeof(*carrier));
    if (carrier != NULL) {
      carrier->sqe.user_data = user_data;
      carrier->cqe_flags = flags;
      backlog_locked(r, carrier, res);
    } else {
      cq_of(r)->overflow++;
    }
  }
  mtx_unlock(&r->mtx);
}

/* ---- the fixed file table ---------------------------------------------
 * Slots holding real descriptors, the alloc range naming which slots a
 * direct accept may pick. Created by register before traffic, mutated
 * only on the engine thread after that. */

int slip_fixed_lookup(struct slip_ring *r, int slot) {
  if (slot < 0 || (unsigned) slot >= r->fixed_n || r->fixed[slot] < 0) return -EBADF;
  return r->fixed[slot];
}

int slip_fixed_install(struct slip_ring *r, __u32 file_index, int realfd) {
  if (r->fixed == NULL) return -ENXIO;
  if (file_index == IORING_FILE_INDEX_ALLOC) {
    const unsigned end = r->alloc_off + r->alloc_len;
    unsigned probe = r->alloc_hint;
    if (probe < r->alloc_off || probe >= end) probe = r->alloc_off;
    for (unsigned tried = 0; tried < r->alloc_len; tried++) {
      if (r->fixed[probe] < 0) {
        r->fixed[probe] = realfd;
        r->alloc_hint = probe + 1;
        return (int) probe;
      }
      if (++probe >= end) probe = r->alloc_off;
    }
    return -ENFILE; /* the alloc range is full - the kernel's word */
  }
  const unsigned slot = file_index - 1;
  if (slot >= r->fixed_n) return -EBADF;
  /* The kernel REPLACES an occupied slot and closes what it held; no
   * consumer of this engine does, and this file holds no OS call to
   * close with - refused until someone needs it. */
  if (r->fixed[slot] >= 0) return -EBUSY;
  r->fixed[slot] = realfd;
  return (int) slot;
}

int slip_fixed_clear(struct slip_ring *r, unsigned slot) {
  if (r->fixed == NULL || slot >= r->fixed_n || r->fixed[slot] < 0) return -EBADF;
  r->fixed[slot] = -1;
  if (slot >= r->alloc_off && slot < r->alloc_hint) r->alloc_hint = slot;
  return 0;
}

struct slip_bufring *slip_bufring_of(struct slip_ring *r, unsigned short bgid) {
  for (int i = 0; i < SLIP_BUFRINGS_MAX; i++) {
    if (r->bufrings[i].in_use && r->bufrings[i].bgid == bgid) return &r->bufrings[i];
  }
  return NULL;
}

/* ---- the engine thread ------------------------------------------------ */

/* An op is done: settle its chain standing, then post. Engine thread
 * only - the worker's path settles through the ticket instead. */
static void complete(struct slip_ring *r, struct eng_op *op, int res) {
  if (op == r->blocking) {
    r->blocking = NULL;
    if (res < 0) r->chain_failed = 1;
  } else if (res < 0 && (op->sqe.flags & IOSQE_IO_LINK)) {
    r->chain_failed = 1;
  }
  slip_engine_post(r, op, res);
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
      slip_engine_post(r, op, -ECANCELED);
      if (!linked) r->chain_failed = 0;
      continue;
    }

    /* IOSQE_FIXED_FILE resolves HERE, once, before any backend sees the
     * op: the slot becomes the real descriptor and the flag comes off,
     * so parking, retries, cancel-by-fd and the worker all speak real
     * descriptors. SOCKET is the one op whose fd field is not a
     * descriptor (it carries the domain) and NOP has none - neither is
     * translated. The install ops keep their table side in file_index,
     * which survives untouched. */
    if ((op->sqe.flags & IOSQE_FIXED_FILE) && op->sqe.opcode != IORING_OP_SOCKET &&
        op->sqe.opcode != IORING_OP_NOP) {
      const int realfd = slip_fixed_lookup(r, op->sqe.fd);
      if (realfd < 0) {
        if (linked) r->chain_failed = 1;
        slip_engine_post(r, op, -EBADF);
        continue;
      }
      op->sqe.fd = realfd;
      op->sqe.flags &= (__u8) ~IOSQE_FIXED_FILE;
    }

    int res = 0;
    if (r->be->execute(r, op, &res) == EXEC_DONE) {
      if (res < 0 && linked) r->chain_failed = 1;
      slip_engine_post(r, op, res);
    } else if (linked) {
      r->blocking = op; /* the chain waits wherever the op is */
    }
  }
}

static int engine_main(void *arg) {
  struct slip_ring *r = arg;
  struct eng_done done[SLIP_WAITING_MAX];

  for (;;) {
    const int n = r->be->wait(r, done, SLIP_WAITING_MAX);
    if (n < 0) break;

    /* Wakeups are level-noisy and pokes coalesce, so every wakeup does
     * the whole drain: shutdown, new submissions, the worker's ticket,
     * backlog room. */
    mtx_lock(&r->mtx);
    if (r->stopping) {
      mtx_unlock(&r->mtx);
      break;
    }
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

    for (int i = 0; i < n; i++) complete(r, done[i].op, done[i].res);

    process_queue(r);
  }
  return 0;
}

/* ---- the exported five, and the backend switch ------------------------ */

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
  if (g_backend == NULL) g_backend = backend_default();
  r->be = g_backend;
  r->be_fd = -1;
  r->ctl_r = r->ctl_w = -1;
  r->sq_entries = round_up_pow2(entries);
  r->cq_entries = p->cq_entries ? round_up_pow2(p->cq_entries) : r->sq_entries * 2;

  r->sq_size = sizeof(struct sq_ring) + (size_t) r->sq_entries * sizeof(unsigned);
  r->cq_size = sizeof(struct cq_ring) + (size_t) r->cq_entries * sizeof(struct io_uring_cqe);
  r->sqes_size = (size_t) r->sq_entries * sizeof(struct io_uring_sqe);

  r->sq_block = calloc(1, r->sq_size);
  r->cq_block = calloc(1, r->cq_size);
  r->sqes = calloc(r->sq_entries, sizeof(struct io_uring_sqe));
  if (!r->sq_block || !r->cq_block || !r->sqes ||
      mtx_init(&r->mtx, mtx_plain) != thrd_success ||
      cnd_init(&r->cv) != thrd_success || cnd_init(&r->wq_cv) != thrd_success) {
    free(r->sq_block); free(r->cq_block); free(r->sqes);
    memset(r, 0, sizeof(*r));
    return -ENOMEM;
  }

  if (r->be->open_ring(r) != 0) {
    mtx_destroy(&r->mtx);
    cnd_destroy(&r->cv);
    cnd_destroy(&r->wq_cv);
    free(r->sq_block); free(r->cq_block); free(r->sqes);
    memset(r, 0, sizeof(*r));
    return -ENOMEM;
  }

  sq_of(r)->ring_mask = r->sq_entries - 1;
  sq_of(r)->ring_entries = r->sq_entries;
  cq_of(r)->ring_mask = r->cq_entries - 1;
  cq_of(r)->ring_entries = r->cq_entries;

  if (thrd_create(&r->engine, engine_main, r) != thrd_success) {
    r->be->close_ring(r);
    mtx_destroy(&r->mtx);
    cnd_destroy(&r->cv);
    cnd_destroy(&r->wq_cv);
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

/* The register family, grown op by op like the ring ops - and first of
 * all the PROBE, because it is how liburing itself asks what a ring can
 * do: io_uring_get_probe drives one, and mruby-io-uring gates
 * URING_AVAILABLE on its answer. The engine reports its backend's
 * carried list and nothing more - the probe tells the truth.
 *
 * The rest is what webmachine's ring setup performs, in its order:
 * the sparse fixed file table, the alloc range direct accepts pick
 * from, the ring's own registration for enter, and the provided-buffer
 * ring the recvs select from. Each registers STATE; the ops that use it
 * live in the backends. Registration happens before traffic and
 * unregistration after it (SINGLE_ISSUER's shape); the engine thread
 * synchronizes with all of it through the inbox handoff. Everything
 * else answers -EINVAL, the kernel's word for a register opcode it does
 * not know. */
int slipstream_engine_register(int fd, unsigned int opcode, void *arg,
                               unsigned int nr_args) {
  struct slip_ring *r = ring_of(fd);
  if (r == NULL) return -EBADF;
  switch (opcode) {
    case IORING_REGISTER_PROBE: {
      if (arg == NULL) return -EFAULT;
      struct io_uring_probe *p = arg;
      memset(p, 0, sizeof(*p) + (size_t) nr_args * sizeof(struct io_uring_probe_op));
      unsigned last = 0;
      for (const unsigned char *c = r->be->carried_ops; *c != 255; c++) {
        if (*c > last) last = *c;
        if (*c < nr_args) p->ops[*c].flags = IO_URING_OP_SUPPORTED;
      }
      p->last_op = (__u8) last;
      for (unsigned i = 0; i < nr_args; i++) p->ops[i].op = (__u8) i;
      p->ops_len = (__u8) (nr_args < 256 ? nr_args : 255);
      return 0;
    }
    case IORING_REGISTER_FILES2: {
      /* Sparse only - a table of empty slots for direct installs, which
       * is the one shape the consumers ask for (register_files_sparse).
       * A table of caller descriptors is refused until someone needs
       * it. */
      if (arg == NULL) return -EFAULT;
      const struct io_uring_rsrc_register *rr = arg;
      if (!(rr->flags & IORING_RSRC_REGISTER_SPARSE) || rr->data != 0 || rr->tags != 0)
        return -EINVAL;
      if (rr->nr == 0 || rr->nr > (1u << 20)) return -EINVAL;
      if (r->fixed != NULL) return -EBUSY; /* one table per ring, like the kernel */
      int *table = malloc((size_t) rr->nr * sizeof(int));
      if (table == NULL) return -ENOMEM;
      for (unsigned i = 0; i < rr->nr; i++) table[i] = -1;
      r->fixed = table;
      r->fixed_n = rr->nr;
      r->alloc_off = 0;
      r->alloc_len = rr->nr;
      r->alloc_hint = 0;
      return 0;
    }
    case IORING_UNREGISTER_FILES: {
      if (r->fixed == NULL) return -ENXIO;
      for (unsigned i = 0; i < r->fixed_n; i++) {
        if (r->fixed[i] >= 0) slip_native_fd_close(r->fixed[i]);
      }
      free(r->fixed);
      r->fixed = NULL;
      r->fixed_n = r->alloc_off = r->alloc_len = r->alloc_hint = 0;
      return 0;
    }
    case IORING_REGISTER_FILE_ALLOC_RANGE: {
      if (arg == NULL) return -EFAULT;
      const struct io_uring_file_index_range *range = arg;
      if (r->fixed == NULL) return -ENXIO;
      if (range->resv != 0 || range->off > r->fixed_n ||
          range->len > r->fixed_n - range->off)
        return -EINVAL;
      r->alloc_off = range->off;
      r->alloc_len = range->len;
      r->alloc_hint = range->off;
      return 0;
    }
    case IORING_REGISTER_RING_FDS: {
      /* The kernel hands back a table offset that enter then names with
       * IORING_ENTER_REGISTERED_RING. The engine's rings are already
       * tokens, so the token IS the offset - enter resolves it the same
       * way with or without the flag, and nothing needs a table. */
      if (arg == NULL) return -EFAULT;
      struct io_uring_rsrc_update *up = arg;
      for (unsigned i = 0; i < nr_args; i++) up[i].offset = (__u32) fd;
      return (int) nr_args;
    }
    case IORING_UNREGISTER_RING_FDS:
      return (int) nr_args;
    case IORING_REGISTER_PBUF_RING: {
      if (arg == NULL) return -EFAULT;
      const struct io_uring_buf_reg *reg = arg;
      /* IOU_PBUF_RING_MMAP wants the engine to own the memory, and the
       * incremental flag a second protocol - neither consumer asks;
       * refused, not half-served. */
      if (reg->flags != 0 || reg->ring_addr == 0) return -EINVAL;
      if (reg->ring_entries == 0 || (reg->ring_entries & (reg->ring_entries - 1)) != 0 ||
          reg->ring_entries > (1u << 15))
        return -EINVAL;
      if (slip_bufring_of(r, reg->bgid) != NULL) return -EEXIST;
      for (int i = 0; i < SLIP_BUFRINGS_MAX; i++) {
        struct slip_bufring *b = &r->bufrings[i];
        if (b->in_use) continue;
        b->bufs = (struct io_uring_buf *) (uintptr_t) reg->ring_addr;
        b->entries = reg->ring_entries;
        b->bgid = reg->bgid;
        b->head = 0;
        b->in_use = 1;
        return 0;
      }
      return -ENOMEM;
    }
    case IORING_UNREGISTER_PBUF_RING: {
      if (arg == NULL) return -EFAULT;
      const struct io_uring_buf_reg *reg = arg;
      struct slip_bufring *b = slip_bufring_of(r, reg->bgid);
      if (b == NULL) return -ENOENT;
      memset(b, 0, sizeof(*b));
      return 0;
    }
    default:
      return -EINVAL;
  }
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
  r->be->poke(r);
  if (r->engine_live) thrd_join(r->engine, NULL);
  if (r->worker_live) thrd_join(r->worker, NULL);
  r->be->close_ring(r);

  free_op_list(r->inbox_head);
  free_op_list(r->backlog_head);
  free_op_list(r->wq_head);
  free_op_list(r->queue_head);
  for (unsigned i = 0; i < r->waiting_n; i++) free(r->waiting[i]);
  if (r->fixed != NULL) {
    /* The ring held these descriptors the way the kernel holds
     * registered files - the ring's exit is their close. */
    for (unsigned i = 0; i < r->fixed_n; i++) {
      if (r->fixed[i] >= 0) slip_native_fd_close(r->fixed[i]);
    }
    free(r->fixed);
  }
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
    r->be->poke(r);
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
