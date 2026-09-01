/* THE FLOOR: select(2) over the parked set, rebuilt on every wait.
 *
 * This is the backend that exists so there is always one. select is
 * older than every alternative and present wherever sockets are - no
 * epoll, no kqueue, no ports, no dispatch. A platform this engine has
 * never seen still has this.
 *
 * Stateless, so arm and disarm have nothing to keep: membership in
 * waiting[] IS the interest set, and two ops on one descriptor cost
 * nothing because the sets are bitmaps. The price is that every wait
 * rebuilds them and every answer is read back by scanning - which is
 * what select is, and why the platforms that have something better use
 * it instead.
 *
 * FD_SETSIZE is a hard ceiling here, not a suggestion: a descriptor at
 * or above it cannot be put in an fd_set at all, and writing it anyway
 * is memory corruption. Such a descriptor is refused when it parks, so
 * the caller gets an error instead of a silent wrong answer. */
#ifndef _WIN32

#include "engine_internal.h"

#include <poll.h>
#include <sys/select.h>

static int select_open_ring(struct slip_ring *r) { return slip_posix_ctl_open(r); }
static void select_close_ring(struct slip_ring *r) { slip_posix_ctl_close(r); }

/* The one thing this backend must check: an fd_set holds descriptors
 * below FD_SETSIZE and nothing else. */
static int select_arm(struct slip_ring *r, struct eng_op *op) {
  (void) r;
  return op->sqe.fd >= 0 && op->sqe.fd < FD_SETSIZE ? 0 : -1;
}

static void select_disarm(struct slip_ring *r, struct eng_op *op) {
  (void) r;
  (void) op;
}

static int select_wait(struct slip_ring *r, struct eng_done *out, unsigned max,
                       int timeout_ms) {
  fd_set rd, wr;
  FD_ZERO(&rd);
  FD_ZERO(&wr);
  int top = r->ctl_r;
  FD_SET(r->ctl_r, &rd);
  for (unsigned i = 0; i < r->waiting_n; i++) {
    const int fd = r->waiting[i]->sqe.fd;
    if (fd < 0 || fd >= FD_SETSIZE) continue; /* refused at arm; never here */
    if (r->waiting[i]->wait_events & POLLOUT) FD_SET(fd, &wr);
    else FD_SET(fd, &rd);
    if (fd > top) top = fd;
  }

  struct timeval tv;
  if (timeout_ms >= 0) {
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
  }
  if (select(top + 1, &rd, &wr, NULL, timeout_ms >= 0 ? &tv : NULL) < 0)
    return 0; /* EINTR: a harmless drain */
  if (FD_ISSET(r->ctl_r, &rd)) slip_posix_ctl_drain(r);

  /* select answers in the sets it was given, so the parked ops are
   * walked once to read the answer off. There is nowhere else to put
   * it - an fd_set has no room for who was waiting. */
  struct eng_op *ready[SLIP_WAITING_MAX];
  unsigned n = 0;
  for (unsigned i = 0; i < r->waiting_n && n < SLIP_WAITING_MAX; i++) {
    struct eng_op *op = r->waiting[i];
    const int fd = op->sqe.fd;
    if (fd < 0 || fd >= FD_SETSIZE) continue;
    if (FD_ISSET(fd, (op->wait_events & POLLOUT) ? &wr : &rd)) ready[n++] = op;
  }
  return slip_posix_finish_ready(r, ready, n, out, max);
}

const struct eng_backend slip_backend_select = {
  .name = "select",
  .open_ring = select_open_ring,
  .close_ring = select_close_ring,
  .poke = slip_posix_poke,
  .execute = slip_posix_execute,
  .wait = select_wait,
  .carried_ops = slip_posix_carried_ops,
  .arm = select_arm,
  .disarm = select_disarm,
};

#else
typedef int slip_backend_select_is_posix_only;
#endif
