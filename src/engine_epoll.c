/* The Linux backend: the parked set held by the kernel, level
 * triggered. A delivery is resolved back to ops by scanning waiting[] -
 * the array is the truth, the epoll set its mirror.
 *
 * REGISTRATIONS STAY. Arming on every park and dropping on every
 * unpark cost two epoll_ctl per request - measured with strace under
 * load, 34.8% of the server's syscall time, on a connection that parks
 * and unparks for every single request it serves. Level triggering is
 * what makes leaving them in place safe: a descriptor is reported only
 * while it is READABLE, and a drained socket is not, so a registration
 * with nothing parked behind it stays quiet by itself. The one case it
 * does not - bytes arriving while no op waits for them - is answered in
 * wait() by dropping that descriptor, which is a syscall in the rare
 * case instead of two in the common one.
 *
 * Interest is therefore a CACHE, not a recomputation: what the kernel
 * was last told, per descriptor. A park whose direction is already
 * covered costs nothing at all - no syscall, and no scan of waiting[].
 * The cache can go stale exactly once, when a descriptor is closed and
 * its number reused; MOD then answers -ENOENT and the ADD fallback puts
 * it right.
 *
 * This backend exists for the case the engine exists for on Linux at
 * all: io_uring refused at runtime (seccomp, io_uring_disabled) on the
 * platform whose native readiness API this is. */
#ifdef __linux__

#include "engine_internal.h"

#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

/* What the kernel was last told, per descriptor; 0 means not in the
 * set. Indexed by the descriptor itself - they are small integers by
 * definition, and the table grows to the largest one seen. */
struct ep_state {
  uint32_t *interest;
  unsigned n;
};

static uint32_t ep_events_of(short wait_events) {
  uint32_t e = 0;
  if (wait_events & POLLIN) e |= EPOLLIN;
  if (wait_events & POLLOUT) e |= EPOLLOUT;
  return e;
}

static uint32_t *ep_slot(struct slip_ring *r, int fd) {
  struct ep_state *st = r->be_state;
  if (fd < 0) return NULL;
  if ((unsigned) fd >= st->n) {
    unsigned want = st->n ? st->n : 64;
    while (want <= (unsigned) fd) want *= 2;
    uint32_t *grown = realloc(st->interest, (size_t) want * sizeof(*grown));
    if (grown == NULL) return NULL;
    memset(grown + st->n, 0, (size_t) (want - st->n) * sizeof(*grown));
    st->interest = grown;
    st->n = want;
  }
  return &st->interest[fd];
}

/* 1 when some parked op still wants this descriptor. */
static int ep_still_wanted(struct slip_ring *r, int fd) {
  for (unsigned i = 0; i < r->waiting_n; i++) {
    if (r->waiting[i]->sqe.fd == fd) return 1;
  }
  return 0;
}

/* Out of the set, and out of the cache with it. */
static void ep_drop(struct slip_ring *r, int fd) {
  epoll_ctl(r->be_fd, EPOLL_CTL_DEL, fd, NULL);
  uint32_t *slot = ep_slot(r, fd);
  if (slot != NULL) *slot = 0;
}

static int epoll_open_ring(struct slip_ring *r) {
  if (slip_posix_ctl_open(r) != 0) return -1;
  struct ep_state *st = calloc(1, sizeof(*st));
  if (st == NULL) {
    slip_posix_ctl_close(r);
    return -1;
  }
  r->be_state = st;
  r->be_fd = epoll_create1(EPOLL_CLOEXEC);
  if (r->be_fd < 0) {
    free(st);
    r->be_state = NULL;
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
  struct ep_state *st = r->be_state;
  if (st != NULL) {
    free(st->interest);
    free(st);
    r->be_state = NULL;
  }
  slip_posix_ctl_close(r);
}

/* Nothing to do when the kernel already watches this direction - the
 * usual case for a connection that parks a read after every request. */
static int epoll_arm(struct slip_ring *r, struct eng_op *op) {
  uint32_t *slot = ep_slot(r, op->sqe.fd);
  if (slot == NULL) return -1;
  const uint32_t want = ep_events_of(op->wait_events);
  if ((*slot & want) == want && *slot != 0) return 0;

  const uint32_t merged = *slot | want;
  struct epoll_event ev = { .events = merged, .data.fd = op->sqe.fd };
  const int op_first = *slot == 0 ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;
  if (epoll_ctl(r->be_fd, op_first, op->sqe.fd, &ev) == 0) {
    *slot = merged;
    return 0;
  }
  /* The cache and the kernel disagree - a closed descriptor whose
   * number came back. Whichever way round it is, the other call is
   * the right one. */
  const int op_other = op_first == EPOLL_CTL_ADD ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
  if ((errno == EEXIST || errno == ENOENT) &&
      epoll_ctl(r->be_fd, op_other, op->sqe.fd, &ev) == 0) {
    *slot = merged;
    return 0;
  }
  *slot = 0;
  return -1;
}

/* The registration STAYS: a drained descriptor reports nothing under
 * level triggering, so leaving it costs nothing, and re-arming it for
 * the next request then costs nothing either. */
static void epoll_disarm(struct slip_ring *r, struct eng_op *op) {
  (void) r;
  (void) op;
}

static int epoll_wait_ready(struct slip_ring *r, struct eng_done *out, unsigned max,
                            int timeout_ms) {
  struct epoll_event evs[64];
  const int got = epoll_wait(r->be_fd, evs, 64, timeout_ms);
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
    unsigned before = n;
    for (unsigned i = 0; i < r->waiting_n && n < SLIP_WAITING_MAX; i++) {
      struct eng_op *op = r->waiting[i];
      if (op->sqe.fd == evs[e].data.fd &&
          (fired & (op->wait_events | POLLERR | POLLHUP)))
        ready[n++] = op;
    }
    /* Ready, and nobody waiting for it: a registration that outlived
     * its ops. Left in place it would report on every wait and spin, so
     * it goes now - and comes back the moment something parks on it. */
    if (n == before && !ep_still_wanted(r, evs[e].data.fd)) ep_drop(r, evs[e].data.fd);
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
