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
struct ep_fd {
  uint32_t interest; /* 0: not in the set */
  struct eng_op *in;
  struct eng_op *out;
};

struct ep_state {
  struct ep_fd *fds;
  unsigned n;
};

static uint32_t ep_events_of(short wait_events) {
  uint32_t e = 0;
  if (wait_events & POLLIN) e |= EPOLLIN;
  if (wait_events & POLLOUT) e |= EPOLLOUT;
  if (wait_events & POLLPRI) e |= EPOLLPRI; /* out-of-band has its own bit here */
  return e;
}

static struct ep_fd *ep_slot(struct slip_ring *r, int fd) {
  struct ep_state *st = r->be_state;
  if (fd < 0) return NULL;
  if ((unsigned) fd >= st->n) {
    unsigned want = st->n ? st->n : 64;
    while (want <= (unsigned) fd) want *= 2;
    struct ep_fd *grown = realloc(st->fds, (size_t) want * sizeof(*grown));
    if (grown == NULL) return NULL;
    memset(grown + st->n, 0, (size_t) (want - st->n) * sizeof(*grown));
    st->fds = grown;
    st->n = want;
  }
  return &st->fds[fd];
}

/* Out of the set, and out of the cache with it. */
/* Stop asking about a direction nobody is waiting for. LEVEL TRIGGERED
 * means a descriptor with bytes still in it is reported on EVERY wait,
 * so a registration that outlives its ops does not go quiet - it
 * spins. It stays quiet only while it is drained, and the caller is
 * under no obligation to drain it: a recv that took half the bytes and
 * completed leaves the rest sitting there.
 *
 * So the side that fired with nobody behind it loses its bit, and a
 * descriptor that has no bits left leaves the set. Both cost one call,
 * in the case that would otherwise burn the core, and the next park
 * pays one to come back. */
static void ep_quiet(struct slip_ring *r, struct ep_fd *slot, int fd, uint32_t unwanted) {
  const uint32_t left = slot->interest & ~unwanted;
  if (left == 0) {
    epoll_ctl(r->be_fd, EPOLL_CTL_DEL, fd, NULL);
    slot->interest = 0;
    return;
  }
  struct epoll_event ev = { .events = left, .data.fd = fd };
  if (epoll_ctl(r->be_fd, EPOLL_CTL_MOD, fd, &ev) == 0) slot->interest = left;
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
    free(st->fds);
    free(st);
    r->be_state = NULL;
  }
  slip_posix_ctl_close(r);
}

/* Nothing to do when the kernel already watches this direction - the
 * usual case for a connection that parks a read after every request. */
static int epoll_arm(struct slip_ring *r, struct eng_op *op) {
  struct ep_fd *slot = ep_slot(r, op->sqe.fd);
  if (slot == NULL) return -1;
  const uint32_t want = ep_events_of(op->wait_events);

  /* The op joins its side of this descriptor - that is what makes a
   * delivery answerable without searching for it later. */
  struct eng_op **side = (op->wait_events & POLLOUT) ? &slot->out : &slot->in;
  op->next = *side;
  *side = op;

  if (slot->interest != 0 && (slot->interest & want) == want) return 0;

  const uint32_t merged = slot->interest | want;
  struct epoll_event ev = { .events = merged, .data.fd = op->sqe.fd };
  const int first = slot->interest == 0 ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;
  if (epoll_ctl(r->be_fd, first, op->sqe.fd, &ev) == 0) {
    slot->interest = merged;
    return 0;
  }
  /* The table and the kernel disagree - a closed descriptor whose
   * number came back. Whichever way round it is, the other call is
   * the right one. */
  const int other = first == EPOLL_CTL_ADD ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
  if ((errno == EEXIST || errno == ENOENT) &&
      epoll_ctl(r->be_fd, other, op->sqe.fd, &ev) == 0) {
    slot->interest = merged;
    return 0;
  }
  *side = op->next;
  op->next = NULL;
  slot->interest = 0;
  return -1;
}

/* The registration STAYS: a drained descriptor reports nothing under
 * level triggering, so leaving it costs nothing, and re-arming it for
 * the next request then costs nothing either. */
static void epoll_disarm(struct slip_ring *r, struct eng_op *op) {
  struct ep_fd *slot = ep_slot(r, op->sqe.fd);
  if (slot == NULL) return;
  struct eng_op **side = (op->wait_events & POLLOUT) ? &slot->out : &slot->in;
  while (*side != NULL) {
    if (*side == op) {
      *side = op->next;
      op->next = NULL;
      return;
    }
    side = &(*side)->next;
  }
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
    struct ep_fd *slot = ep_slot(r, evs[e].data.fd);
    if (slot == NULL) continue;
    /* NO SEARCH: the descriptor that woke hands over its own waiters.
     * An error or a hangup wakes both sides - what it means is what
     * the retry finds out, the way poll reports them regardless. */
    const int broken = (evs[e].events & (EPOLLERR | EPOLLHUP)) != 0;
    if ((evs[e].events & EPOLLIN) || broken) {
      for (struct eng_op *op = slot->in; op != NULL && n < SLIP_WAITING_MAX; op = op->next)
        ready[n++] = op;
    }
    if ((evs[e].events & EPOLLOUT) || broken) {
      for (struct eng_op *op = slot->out; op != NULL && n < SLIP_WAITING_MAX; op = op->next)
        ready[n++] = op;
    }
    /* Whatever fired with nobody behind it is asked about no further -
     * per SIDE, because one registration carries both and a writer
     * waiting on a socket that still holds unread bytes would otherwise
     * be woken about those bytes forever. */
    uint32_t unwanted = 0;
    if ((evs[e].events & EPOLLIN) && slot->in == NULL) unwanted |= EPOLLIN | EPOLLPRI;
    if ((evs[e].events & EPOLLOUT) && slot->out == NULL) unwanted |= EPOLLOUT;
    if (broken && slot->in == NULL && slot->out == NULL) unwanted = slot->interest;
    if (unwanted != 0) ep_quiet(r, slot, evs[e].data.fd, unwanted);
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
