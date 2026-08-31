/* The Linux backend: the parked set held by the kernel, level
 * triggered. epoll registers a descriptor once, so interest is the OR
 * of every parked op on that descriptor, recomputed when one joins or
 * leaves, and a delivery is resolved back to ops by scanning waiting[]
 * - the array is the truth, the epoll set its mirror.
 *
 * This backend exists for the case the engine exists for on Linux at
 * all: io_uring refused at runtime (seccomp, io_uring_disabled) on the
 * platform whose native readiness API this is. */
#ifdef __linux__

#include "engine_internal.h"

#include <poll.h>
#include <sys/epoll.h>
#include <unistd.h>

static uint32_t ep_events_of(short wait_events) {
  uint32_t e = 0;
  if (wait_events & POLLIN) e |= EPOLLIN;
  if (wait_events & POLLOUT) e |= EPOLLOUT;
  return e;
}

static uint32_t ep_merged(struct slip_ring *r, int fd) {
  uint32_t e = 0;
  for (unsigned i = 0; i < r->waiting_n; i++) {
    if (r->waiting[i]->sqe.fd == fd) e |= ep_events_of(r->waiting[i]->wait_events);
  }
  return e;
}

static int epoll_open_ring(struct slip_ring *r) {
  if (slip_posix_ctl_open(r) != 0) return -1;
  r->be_fd = epoll_create1(EPOLL_CLOEXEC);
  if (r->be_fd < 0) {
    slip_posix_ctl_close(r);
    return -1;
  }
  struct epoll_event ev = { .events = EPOLLIN, .data.fd = r->ctl_r };
  if (epoll_ctl(r->be_fd, EPOLL_CTL_ADD, r->ctl_r, &ev) != 0) {
    close(r->be_fd);
    r->be_fd = -1;
    slip_posix_ctl_close(r);
    return -1;
  }
  return 0;
}

static void epoll_close_ring(struct slip_ring *r) {
  if (r->be_fd >= 0) close(r->be_fd);
  r->be_fd = -1;
  slip_posix_ctl_close(r);
}

/* The op is not yet in waiting[]: the merge is the others plus it. ADD
 * first - the common case is a descriptor parked once - and EEXIST
 * falls back to MOD. */
static int epoll_arm(struct slip_ring *r, struct eng_op *op) {
  struct epoll_event ev = {
    .events = ep_merged(r, op->sqe.fd) | ep_events_of(op->wait_events),
    .data.fd = op->sqe.fd,
  };
  if (epoll_ctl(r->be_fd, EPOLL_CTL_ADD, op->sqe.fd, &ev) == 0) return 0;
  if (errno == EEXIST && epoll_ctl(r->be_fd, EPOLL_CTL_MOD, op->sqe.fd, &ev) == 0) return 0;
  return -1;
}

/* The op is already out of waiting[]: what remains is the merge of the
 * rest - nothing left means the descriptor leaves the set. */
static void epoll_disarm(struct slip_ring *r, struct eng_op *op) {
  const uint32_t rest = ep_merged(r, op->sqe.fd);
  if (rest == 0) {
    epoll_ctl(r->be_fd, EPOLL_CTL_DEL, op->sqe.fd, NULL);
  } else {
    struct epoll_event ev = { .events = rest, .data.fd = op->sqe.fd };
    epoll_ctl(r->be_fd, EPOLL_CTL_MOD, op->sqe.fd, &ev);
  }
}

static int epoll_wait_ready(struct slip_ring *r, struct eng_done *out, unsigned max) {
  struct epoll_event evs[64];
  const int got = epoll_wait(r->be_fd, evs, 64, -1);
  if (got < 0) return 0; /* EINTR: a harmless drain */

  struct eng_op *ready[SLIP_WAITING_MAX];
  unsigned n = 0;
  for (int e = 0; e < got; e++) {
    if (evs[e].data.fd == r->ctl_r) {
      slip_posix_ctl_drain(r);
      continue;
    }
    short fired = 0;
    if (evs[e].events & EPOLLIN) fired |= POLLIN;
    if (evs[e].events & EPOLLOUT) fired |= POLLOUT;
    if (evs[e].events & (EPOLLERR | EPOLLHUP)) fired |= POLLERR | POLLHUP;
    for (unsigned i = 0; i < r->waiting_n && n < SLIP_WAITING_MAX; i++) {
      struct eng_op *op = r->waiting[i];
      if (op->sqe.fd == evs[e].data.fd &&
          (fired & (op->wait_events | POLLERR | POLLHUP)))
        ready[n++] = op;
    }
  }
  return slip_posix_finish_ready(r, ready, n, out, max);
}

const struct eng_backend slip_backend_epoll = {
  .name = "epoll",
  .open_ring = epoll_open_ring,
  .close_ring = epoll_close_ring,
  .poke = slip_posix_poke,
  .execute = slip_posix_execute,
  .wait = epoll_wait_ready,
  .carried_ops = slip_posix_carried_ops,
  .arm = epoll_arm,
  .disarm = epoll_disarm,
};

#else
typedef int slip_backend_epoll_is_linux_only;
#endif
