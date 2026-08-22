/* The header is C, and this is the proof: a C11 translation unit, no
 * feature macros on the command line, <liburing.h> included first the
 * way a consumer would. It opens a ring, runs one op and waits for it -
 * enough to instantiate every static inline the API needs and to start
 * and join the engine thread. Real liburing.h is consumable from C; a
 * stand-in that is not would be no stand-in. */
#include <liburing.h>

#include <stdio.h>

int main(void) {
  struct io_uring r;
  struct io_uring_sqe *s;
  struct io_uring_cqe *c = NULL;
  int rc = io_uring_queue_init(8, &r, 0);
  if (rc != 0) {
    printf("queue_init: %d\n", rc);
    return 1;
  }
  if (r.ring_fd < 0) {
    printf("ring_fd is not a descriptor\n");
    return 2;
  }
  s = io_uring_get_sqe(&r);
  if (s == NULL) return 3;
  io_uring_prep_nop(s);
  io_uring_sqe_set_data64(s, 7);
  io_uring_submit(&r);
  if (io_uring_wait_cqe(&r, &c) != 0) return 4;
  if (io_uring_cqe_get_data64(c) != 7 || c->res != 0) return 5;
  io_uring_cqe_seen(&r, c);
  io_uring_queue_exit(&r);
  printf("C consumer: ok\n");
  return 0;
}
