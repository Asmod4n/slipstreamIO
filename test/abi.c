/* liburing's own inlines, driving slipstream's implementation of its
 * symbols. Nothing here includes src/liburing.h - the point is that the
 * caller is written against the real header and cannot tell. */
#include <liburing.h>

#include <stdio.h>
#include <string.h>

static int fails = 0;

static void ok(int cond, const char *what) {
  printf("%-52s %s\n", what, cond ? "ok" : "FAIL");
  if (!cond) fails++;
}

int main(void) {
  struct io_uring ring;
  int rc = io_uring_queue_init(8, &ring, 0);
  ok(rc == 0, "io_uring_queue_init through our queue_init_params");
  if (rc != 0) return 1;

  struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);   /* liburing inline */
  ok(sqe != NULL, "io_uring_get_sqe hands out one of our SQEs");
  io_uring_prep_nop(sqe);                                /* liburing inline */
  io_uring_sqe_set_data64(sqe, 0x5115ULL);               /* liburing inline */

  rc = io_uring_submit(&ring);                           /* ours */
  ok(rc == 1, "io_uring_submit reports one submitted");

  struct io_uring_cqe *cqe = NULL;
  rc = io_uring_wait_cqe(&ring, &cqe);                   /* inline -> ours */
  ok(rc == 0 && cqe != NULL, "io_uring_wait_cqe returns a completion");
  ok(cqe && cqe->user_data == 0x5115ULL, "the user_data the caller set comes back");
  ok(cqe && cqe->res == 0, "a NOP completes with res 0");
  io_uring_cq_advance(&ring, 1);                         /* liburing inline */

  rc = io_uring_peek_cqe(&ring, &cqe);                   /* inline -> ours */
  ok(rc != 0, "and the ring is empty again after advancing");

  /* The whole SQ, to prove the mask and the cursors line up. */
  unsigned n = 0;
  while ((sqe = io_uring_get_sqe(&ring)) != NULL) {
    io_uring_prep_nop(sqe);
    io_uring_sqe_set_data64(sqe, 100 + n);
    n++;
  }
  ok(n == 8, "get_sqe fills the ring and then refuses");
  rc = io_uring_submit(&ring);
  ok(rc == 8, "all eight submitted");

  unsigned seen = 0, head;
  io_uring_for_each_cqe(&ring, head, cqe) {             /* liburing inline */
    if (cqe->user_data != 100 + seen) { ok(0, "completions in order"); break; }
    seen++;
  }
  io_uring_cq_advance(&ring, seen);
  ok(seen == 8, "for_each_cqe walks all eight, in order");

  io_uring_queue_exit(&ring);                            /* ours */
  printf("%s\n", fails ? "FAILURES" : "all ok");
  return fails ? 1 : 0;
}
