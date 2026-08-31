/* The portable backend: poll(2) over the parked set, rebuilt on every
 * wait. Stateless, so arm and disarm have nothing to keep - membership
 * in waiting[] IS the interest set - and duplicates of one descriptor
 * cost nothing, poll takes them as separate rows. */
#ifndef _WIN32

#include "engine_internal.h"

#include <poll.h>

static int poll_open_ring(struct slip_ring *r) { return slip_posix_ctl_open(r); }
static void poll_close_ring(struct slip_ring *r) { slip_posix_ctl_close(r); }

static int poll_arm(struct slip_ring *r, struct eng_op *op) {
  (void) r;
  (void) op;
  return 0;
}

static void poll_disarm(struct slip_ring *r, struct eng_op *op) {
  (void) r;
  (void) op;
}

static int poll_wait(struct slip_ring *r, struct eng_done *out, unsigned max) {
  struct pollfd pfds[SLIP_WAITING_MAX + 1];
  pfds[0].fd = r->ctl_r;
  pfds[0].events = POLLIN;
  for (unsigned i = 0; i < r->waiting_n; i++) {
    pfds[i + 1].fd = r->waiting[i]->sqe.fd;
    pfds[i + 1].events = r->waiting[i]->wait_events;
  }
  if (poll(pfds, r->waiting_n + 1, -1) < 0) return 0; /* EINTR: a harmless drain */
  if (pfds[0].revents & POLLIN) slip_posix_ctl_drain(r);

  struct eng_op *ready[SLIP_WAITING_MAX];
  unsigned n = 0;
  for (unsigned i = 0; i < r->waiting_n; i++) {
    if (pfds[i + 1].revents & (r->waiting[i]->wait_events | POLLERR | POLLHUP))
      ready[n++] = r->waiting[i];
  }
  return slip_posix_finish_ready(r, ready, n, out, max);
}

const struct eng_backend slip_backend_poll = {
  .name = "poll",
  .open_ring = poll_open_ring,
  .close_ring = poll_close_ring,
  .poke = slip_posix_poke,
  .execute = slip_posix_execute,
  .wait = poll_wait,
  .carried_ops = slip_posix_carried_ops,
  .arm = poll_arm,
  .disarm = poll_disarm,
};

#else
typedef int slip_backend_poll_is_posix_only;
#endif
