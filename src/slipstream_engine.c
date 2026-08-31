/* What liburing needs from underneath when there is no kernel to ask.
 *
 * liburing does three things after __sys_io_uring_setup: it maps the two
 * rings and the SQE array at the offsets it was handed, it fills its own
 * struct io_uring from the io_sqring_offsets/io_cqring_offsets we
 * report, and from then on its inlines read and write that memory. We
 * own __sys_mmap too, so none of it has to be a file - the "fd" from
 * setup is a token, and a map of it at one of the three ring offsets
 * hands back the block we already allocated.
 *
 * What the engine then has to understand is one shape, not fifty:
 * io_uring_prep_rw fills opcode, fd, off, addr and len, and 56 of
 * liburing's prep functions go through it. Each op adds at most one
 * field of its own from the union afterwards.
 */
#include "slipstream_engine.h"

#include <errno.h>
#include <linux/io_uring.h>
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define SLIP_RINGS_MAX 64

/* The layout we report, and therefore the one liburing reads through. */
struct sq_ring {
  unsigned head, tail, ring_mask, ring_entries, flags, dropped;
  /* array[] follows */
};
struct cq_ring {
  unsigned head, tail, ring_mask, ring_entries, overflow, flags;
  /* cqes[] follow */
};

struct slip_ring {
  int in_use;
  unsigned sq_entries, cq_entries;
  void *sq_block; /* struct sq_ring + array[sq_entries] */
  void *cq_block; /* struct cq_ring + cqes[cq_entries] */
  struct io_uring_sqe *sqes;
  size_t sq_size, cq_size, sqes_size;
};

static struct slip_ring g_rings[SLIP_RINGS_MAX];

static struct slip_ring *ring_of(int fd) {
  if (fd < 0 || fd >= SLIP_RINGS_MAX || !g_rings[fd].in_use) return NULL;
  return &g_rings[fd];
}

static struct sq_ring *sq_of(struct slip_ring *r) { return (struct sq_ring *) r->sq_block; }
static struct cq_ring *cq_of(struct slip_ring *r) { return (struct cq_ring *) r->cq_block; }
static unsigned *sq_array(struct slip_ring *r) { return (unsigned *) ((char *) r->sq_block + sizeof(struct sq_ring)); }
static struct io_uring_cqe *cq_cqes(struct slip_ring *r) {
  return (struct io_uring_cqe *) ((char *) r->cq_block + sizeof(struct cq_ring));
}

static unsigned round_up_pow2(unsigned v) {
  unsigned n = 1;
  while (n < v) n <<= 1;
  return n;
}

int slipstream_engine_setup(unsigned int entries, struct io_uring_params *p) {
  if (entries == 0 || p == NULL) return -EINVAL;
  /* Shapes that change what liburing's inlines expect of this memory.
   * Refused by name rather than half-served. */
  if (p->flags & (IORING_SETUP_SQPOLL | IORING_SETUP_SQE128 | IORING_SETUP_CQE32 |
                  IORING_SETUP_IOPOLL | IORING_SETUP_NO_MMAP))
    return -EINVAL;

  int fd = -1;
  for (int i = 0; i < SLIP_RINGS_MAX; i++) {
    if (!g_rings[i].in_use) { fd = i; break; }
  }
  if (fd < 0) return -EMFILE;

  struct slip_ring *r = &g_rings[fd];
  memset(r, 0, sizeof(*r));
  r->sq_entries = round_up_pow2(entries);
  r->cq_entries = p->cq_entries ? round_up_pow2(p->cq_entries) : r->sq_entries * 2;

  r->sq_size = sizeof(struct sq_ring) + (size_t) r->sq_entries * sizeof(unsigned);
  r->cq_size = sizeof(struct cq_ring) + (size_t) r->cq_entries * sizeof(struct io_uring_cqe);
  r->sqes_size = (size_t) r->sq_entries * sizeof(struct io_uring_sqe);

  r->sq_block = calloc(1, r->sq_size);
  r->cq_block = calloc(1, r->cq_size);
  r->sqes = calloc(r->sq_entries, sizeof(struct io_uring_sqe));
  if (!r->sq_block || !r->cq_block || !r->sqes) {
    free(r->sq_block); free(r->cq_block); free(r->sqes);
    memset(r, 0, sizeof(*r));
    return -ENOMEM;
  }

  sq_of(r)->ring_mask = r->sq_entries - 1;
  sq_of(r)->ring_entries = r->sq_entries;
  cq_of(r)->ring_mask = r->cq_entries - 1;
  cq_of(r)->ring_entries = r->cq_entries;

  /* The offsets liburing indexes the two blocks by. */
  p->sq_entries = r->sq_entries;
  p->cq_entries = r->cq_entries;
  p->features = 0; /* no FEAT_: liburing then takes its oldest, plainest path */
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
  return fd;
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

int slipstream_engine_munmap(void *addr, size_t length) {
  (void) addr;
  (void) length;
  /* The blocks belong to the ring and go when it does, in exit. */
  return 0;
}

int slipstream_engine_close(int fd) {
  struct slip_ring *r = ring_of(fd);
  if (r == NULL) return -EBADF;
  free(r->sq_block);
  free(r->cq_block);
  free(r->sqes);
  memset(r, 0, sizeof(*r));
  return 0;
}

/* One completion, where the caller has not looked yet. */
static void post(struct slip_ring *r, __u64 user_data, int res, unsigned flags) {
  struct cq_ring *cq = cq_of(r);
  if (cq->tail - cq->head >= r->cq_entries) { cq->overflow++; return; }
  struct io_uring_cqe *c = &cq_cqes(r)[cq->tail & cq->ring_mask];
  c->user_data = user_data;
  c->res = res;
  c->flags = flags;
  __atomic_store_n(&cq->tail, cq->tail + 1, __ATOMIC_RELEASE);
}

/* liburing spells "wherever the descriptor stands" as an offset of -1,
 * and that is the only spelling answered with read/write rather than
 * pread/pwrite - a socket or a pipe has no offset. */
static int off_is_current(__u64 off) { return off == (__u64) -1; }

static void run_one(struct slip_ring *r, const struct io_uring_sqe *s) {
  void *buf = (void *) (uintptr_t) s->addr;
  ssize_t n;
  switch (s->opcode) {
    case IORING_OP_NOP:
      post(r, s->user_data, 0, 0);
      return;
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
    case IORING_OP_CLOSE:
      n = close(s->fd);
      break;
    default:
      /* Known op, not carried here. -EOPNOTSUPP and not -EINVAL: the
       * first says "that op, not here", which is what a caller needs in
       * order to take another route. */
      post(r, s->user_data, -EOPNOTSUPP, 0);
      return;
  }
  post(r, s->user_data, n < 0 ? -errno : (int) n, 0);
}

int slipstream_engine_enter(int fd, unsigned int to_submit, unsigned int min_complete,
                            unsigned int flags) {
  (void) flags;
  struct slip_ring *r = ring_of(fd);
  if (r == NULL) return -EBADF;
  struct sq_ring *sq = sq_of(r);

  /* liburing put the indices in array[] and moved tail; head is ours. */
  unsigned submitted = 0;
  const unsigned tail = __atomic_load_n(&sq->tail, __ATOMIC_ACQUIRE);
  while (sq->head != tail && submitted < to_submit) {
    const unsigned i = sq_array(r)[sq->head & sq->ring_mask];
    if (i < r->sq_entries) run_one(r, &r->sqes[i]);
    sq->head++;
    submitted++;
  }

  /* Everything ran where it was submitted, so anything the caller waits
   * for is already there or is never coming. */
  (void) min_complete;
  return (int) submitted;
}
