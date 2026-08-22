#include <slipstreamio.h>
int main() {
  struct io_uring r;
  io_uring_queue_init(64, &r, 0);
  struct io_uring_sqe* s = io_uring_get_sqe(&r);
  io_uring_prep_nop(s);
  io_uring_sqe_set_data64(s, 42);
  io_uring_submit(&r);
  struct io_uring_cqe* c = nullptr;
  if (io_uring_peek_cqe(&r, &c) != 0) return 1;
  if (io_uring_cqe_get_data64(c) != 42 || c->res != 0) return 2;
  io_uring_cqe_seen(&r, c);
  io_uring_queue_exit(&r);
  return 0;
}
