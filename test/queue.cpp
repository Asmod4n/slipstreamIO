// <poll.h> explicitly: against real liburing nothing pulls it in;
// our own liburing.h only happens to. The test compiles against both.
#include <poll.h>
#include <liburing.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdint>
#include <cstdio>
#include <ctime>

static int64_t elapsed_ms(const struct timespec& a, const struct timespec& b) {
  return (b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
}

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

  // (a) An empty queue has nothing that will ever complete, so a short
  // deadline must run out - and take roughly that long, not return
  // instantly - and report -ETIME with the cqe pointer cleared.
  {
    struct __kernel_timespec ts{0, 150000000};  // 150ms
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    struct io_uring_cqe* oc = reinterpret_cast<struct io_uring_cqe*>(1);  // poisoned
    int rc = io_uring_submit_and_wait_timeout(&r, &oc, 1, &ts, nullptr);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (rc != -ETIME) { std::printf("empty queue: expected -ETIME, got %d\n", rc); return 3; }
    if (oc != nullptr) { std::printf("empty queue: cqe_ptr not cleared on timeout\n"); return 3; }
    int64_t ms = elapsed_ms(t0, t1);
    if (ms < 100 || ms > 2000) {
      std::printf("empty queue: waited %lldms, expected roughly 150ms\n", (long long)ms);
      return 3;
    }
  }

  // (b) Five completions, walked two at a time with a partial advance in
  // between: a tick cut off mid-batch must leave the rest in cq, in
  // order and complete, for the next one (task 1's whole point, #116).
  {
    for (uint64_t i = 0; i < 5; i++) {
      struct io_uring_sqe* ns = io_uring_get_sqe(&r);
      io_uring_prep_nop(ns);
      io_uring_sqe_set_data64(ns, 100 + i);
    }
    io_uring_submit(&r);

    unsigned head;
    struct io_uring_cqe* cqe;
    unsigned seen = 0;
    io_uring_for_each_cqe(&r, head, cqe) {
      if (io_uring_cqe_get_data64(cqe) != 100 + seen) {
        std::printf("batch: entry %u out of order\n", seen);
        return 4;
      }
      seen++;
      if (seen == 2) break;  // interrupted mid-batch, on purpose
    }
    if (seen != 2) { std::printf("batch: saw %u before the break, expected 2\n", seen); return 4; }
    io_uring_cq_advance(&r, seen);  // partial: only the two walked

    unsigned remaining = 0;
    io_uring_for_each_cqe(&r, head, cqe) {
      if (io_uring_cqe_get_data64(cqe) != 100 + 2 + remaining) {
        std::printf("batch: remainder out of order at %u\n", remaining);
        return 4;
      }
      remaining++;
    }
    if (remaining != 3) { std::printf("batch: %u left, expected 3\n", remaining); return 4; }
    io_uring_cq_advance(&r, remaining);
    if (io_uring_peek_cqe(&r, &c) != -EAGAIN) { std::printf("batch: cq not empty after full advance\n"); return 4; }
  }

  // (c) A completion that arrives well before the deadline must return
  // immediately with it, not sit out the full timeout.
  {
    int fds[2];
    if (pipe(fds) != 0) return 5;
    struct io_uring_sqe* ps = io_uring_get_sqe(&r);
    io_uring_prep_poll_add(ps, fds[0], POLLIN);
    io_uring_sqe_set_data64(ps, 999);
    io_uring_submit(&r);  // parks: nothing readable on fds[0] yet

    pid_t kid = fork();
    if (kid == 0) {
      usleep(30000);  // 30ms - far inside the 2s deadline below
      char b = 'x';
      if (write(fds[1], &b, 1) != 1) _exit(1);
      _exit(0);
    }

    struct __kernel_timespec ts{2, 0};  // 2s
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    struct io_uring_cqe* oc = nullptr;
    int rc = io_uring_submit_and_wait_timeout(&r, &oc, 1, &ts, nullptr);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    int status = 0;
    waitpid(kid, &status, 0);
    close(fds[0]);
    close(fds[1]);

    if (WEXITSTATUS(status) != 0) { std::printf("early completion: peer write failed\n"); return 6; }
    if (rc < 0) { std::printf("early completion: submit_and_wait_timeout returned %d\n", rc); return 6; }
    if (oc == nullptr || io_uring_cqe_get_data64(oc) != 999) {
      std::printf("early completion: missing the poll cqe\n");
      return 6;
    }
    io_uring_cqe_seen(&r, oc);
    int64_t ms = elapsed_ms(t0, t1);
    if (ms > 1000) {
      std::printf("early completion: took %lldms, should have returned once ready\n", (long long)ms);
      return 6;
    }
  }

  io_uring_queue_exit(&r);
  return 0;
}
