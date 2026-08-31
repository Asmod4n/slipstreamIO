/* The symbols liburing.so exports, implemented here instead.
 *
 * liburing.h's inline half - io_uring_get_sqe, every io_uring_prep_*,
 * io_uring_cq_advance, io_uring_for_each_cqe - is compiled into the
 * CALLER and does nothing but read and write memory whose shape struct
 * io_uring_sq and io_uring_cq describe. So an implementation does not
 * have to be the kernel: it has to hand out memory of that shape and
 * then run what the caller wrote into it.
 *
 * This file is compiled against the REAL liburing.h and never sees
 * src/liburing.h. Those two describe different machines and must not
 * meet in one translation unit.
 *
 * First slice: IORING_OP_NOP, run where it is submitted. No engine, no
 * thread - just the plumbing the inlines need, proved end to end.
 */
#include <liburing.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Everything the caller's inlines index through, in one allocation.
 * liburing keeps the mmap base in sq.ring_ptr and frees it in
 * queue_exit; this uses the same slot for the same purpose. */
struct slip_ring {
  unsigned sq_head, sq_tail, sq_flags, sq_dropped;
  unsigned cq_head, cq_tail, cq_flags, cq_overflow;
  unsigned *array;
  struct io_uring_sqe *sqes;
  struct io_uring_cqe *cqes;
  unsigned sq_entries, cq_entries;
};

static unsigned round_up_pow2(unsigned v) {
  unsigned n = 1;
  while (n < v) n <<= 1;
  return n;
}

int io_uring_queue_init_params(unsigned entries, struct io_uring *ring,
                               struct io_uring_params *p) {
  if (entries == 0 || ring == NULL || p == NULL) return -EINVAL;
  /* SQPOLL, SQE128 and CQE32 change what the inlines expect of this
   * memory. Refused rather than half-served. */
  if (p->flags & (IORING_SETUP_SQPOLL | IORING_SETUP_SQE128 | IORING_SETUP_CQE32))
    return -EINVAL;

  const unsigned sq_entries = round_up_pow2(entries);
  const unsigned cq_entries = p->cq_entries ? round_up_pow2(p->cq_entries) : sq_entries * 2;

  struct slip_ring *s = calloc(1, sizeof(*s));
  if (s == NULL) return -ENOMEM;
  s->sqes = calloc(sq_entries, sizeof(*s->sqes));
  s->cqes = calloc(cq_entries, sizeof(*s->cqes));
  s->array = calloc(sq_entries, sizeof(*s->array));
  if (s->sqes == NULL || s->cqes == NULL || s->array == NULL) {
    free(s->sqes); free(s->cqes); free(s->array); free(s);
    return -ENOMEM;
  }
  s->sq_entries = sq_entries;
  s->cq_entries = cq_entries;

  memset(ring, 0, sizeof(*ring));
  ring->flags = p->flags;
  ring->ring_fd = ring->enter_ring_fd = -1;

  ring->sq.khead = &s->sq_head;
  ring->sq.ktail = &s->sq_tail;
  ring->sq.kflags = &s->sq_flags;
  ring->sq.kdropped = &s->sq_dropped;
  ring->sq.kring_mask = &s->sq_entries;   /* deprecated, kept non-NULL */
  ring->sq.kring_entries = &s->sq_entries;
  ring->sq.array = s->array;
  ring->sq.sqes = s->sqes;
  ring->sq.ring_mask = sq_entries - 1;
  ring->sq.ring_entries = sq_entries;
  ring->sq.ring_ptr = s;
  ring->sq.ring_sz = 0;

  ring->cq.khead = &s->cq_head;
  ring->cq.ktail = &s->cq_tail;
  ring->cq.kflags = &s->cq_flags;
  ring->cq.koverflow = &s->cq_overflow;
  ring->cq.kring_mask = &s->cq_entries;
  ring->cq.kring_entries = &s->cq_entries;
  ring->cq.cqes = s->cqes;
  ring->cq.ring_mask = cq_entries - 1;
  ring->cq.ring_entries = cq_entries;
  ring->cq.ring_ptr = s;
  ring->cq.ring_sz = 0;

  p->sq_entries = sq_entries;
  p->cq_entries = cq_entries;
  return 0;
}

void io_uring_queue_exit(struct io_uring *ring) {
  struct slip_ring *s = ring->sq.ring_ptr;
  if (s == NULL) return;
  free(s->array);
  free(s->cqes);
  free(s->sqes);
  free(s);
  memset(ring, 0, sizeof(*ring));
}

/* One completion, at the tail the caller has not reached yet. */
static int publish(struct slip_ring *s, __u64 user_data, int res, unsigned flags) {
  if (s->cq_tail - s->cq_head >= s->cq_entries) {
    s->cq_overflow++;
    return -EBUSY;
  }
  struct io_uring_cqe *c = &s->cqes[s->cq_tail & (s->cq_entries - 1)];
  c->user_data = user_data;
  c->res = res;
  c->flags = flags;
  s->cq_tail++;
  return 0;
}

static int run_one(struct slip_ring *s, const struct io_uring_sqe *sqe) {
  switch (sqe->opcode) {
    case IORING_OP_NOP:
      return publish(s, sqe->user_data, 0, 0);
    default:
      return publish(s, sqe->user_data, -EINVAL, 0);
  }
}

int io_uring_submit(struct io_uring *ring) {
  struct slip_ring *s = ring->sq.ring_ptr;
  const unsigned mask = s->sq_entries - 1;
  int submitted = 0;
  while (s->sq_head != ring->sq.sqe_tail) {
    run_one(s, &s->sqes[s->sq_head & mask]);
    s->sq_head++;
    submitted++;
  }
  ring->sq.sqe_head = s->sq_head;
  return submitted;
}

int io_uring_submit_and_wait(struct io_uring *ring, unsigned wait_nr) {
  (void) wait_nr;
  return io_uring_submit(ring);
}

int __io_uring_get_cqe(struct io_uring *ring, struct io_uring_cqe **cqe_ptr,
                       unsigned submit, unsigned wait_nr, sigset_t *sigmask) {
  (void) sigmask;
  struct slip_ring *s = ring->sq.ring_ptr;
  if (submit) io_uring_submit(ring);
  if (s->cq_head == s->cq_tail) {
    *cqe_ptr = NULL;
    return wait_nr ? -ETIME : -EAGAIN;
  }
  *cqe_ptr = &s->cqes[s->cq_head & (s->cq_entries - 1)];
  return 0;
}

/* The convenience liburing exports beside queue_init_params: no params
 * to speak of, and the flags the caller wanted. */
int io_uring_queue_init(unsigned entries, struct io_uring *ring, unsigned flags) {
  struct io_uring_params p;
  memset(&p, 0, sizeof(p));
  p.flags = flags;
  return io_uring_queue_init_params(entries, ring, &p);
}
