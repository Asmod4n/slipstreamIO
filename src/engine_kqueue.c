/* The BSD backend: kqueue holds the parked set. A knote is per
 * descriptor AND filter, so a recv and a send parked on one socket are
 * two registrations with no merging - the merge question epoll answers
 * only arises when two parked ops want the SAME direction of the same
 * descriptor, and then the second registration is the same knote again,
 * which EV_ADD updates harmlessly. Deliveries are resolved against
 * waiting[]; the array is the truth, the kqueue its mirror.
 *
 * Also compiles and runs against libkqueue on Linux - the kqueue API
 * over epoll, the same packaging move this project makes for liburing.h
 * - which is how it is proven without a BSD; test/freebsd_vm.sh is the
 * native proof. */
#if defined(__FreeBSD__) || defined(__DragonFly__) || defined(__NetBSD__) || \
    defined(__OpenBSD__) || defined(__APPLE__) || defined(SLIPSTREAM_HAVE_LIBKQUEUE)

#include "engine_internal.h"

#include <poll.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

static short filter_direction(int filter) {
  return filter == EVFILT_WRITE ? POLLOUT : POLLIN;
}

static int kq_filter_of(short wait_events) {
  return (wait_events & POLLOUT) ? EVFILT_WRITE : EVFILT_READ;
}

/* 1 while some parked op still wants this descriptor and direction. */
static int kq_still_wanted(struct slip_ring *r, int fd, int filter) {
  for (unsigned i = 0; i < r->waiting_n; i++) {
    if (r->waiting[i]->sqe.fd == fd &&
        kq_filter_of(r->waiting[i]->wait_events) == filter)
      return 1;
  }
  return 0;
}

static int kqueue_open_ring(struct slip_ring *r) {
  if (slip_posix_ctl_open(r) != 0) return -1;
  r->be_fd = kqueue();
  if (r->be_fd < 0) {
    slip_posix_ctl_close(r);
    return -1;
  }
  struct kevent ev;
  EV_SET(&ev, (uintptr_t) r->ctl_r, EVFILT_READ, EV_ADD, 0, 0, NULL);
  if (kevent(r->be_fd, &ev, 1, NULL, 0, NULL) != 0) {
    close(r->be_fd);
    r->be_fd = -1;
    slip_posix_ctl_close(r);
    return -1;
  }
  return 0;
}

static void kqueue_close_ring(struct slip_ring *r) {
  if (r->be_fd >= 0) close(r->be_fd);
  r->be_fd = -1;
  slip_posix_ctl_close(r);
}

static int kqueue_arm(struct slip_ring *r, struct eng_op *op) {
  struct kevent ev;
  EV_SET(&ev, (uintptr_t) op->sqe.fd, kq_filter_of(op->wait_events), EV_ADD, 0, 0, NULL);
  return kevent(r->be_fd, &ev, 1, NULL, 0, NULL) == 0 ? 0 : -1;
}

/* The op is already out of waiting[]: the knote goes only when nobody
 * left wants its descriptor and direction. */
static void kqueue_disarm(struct slip_ring *r, struct eng_op *op) {
  const int filter = kq_filter_of(op->wait_events);
  if (kq_still_wanted(r, op->sqe.fd, filter)) return;
  struct kevent ev;
  EV_SET(&ev, (uintptr_t) op->sqe.fd, filter, EV_DELETE, 0, 0, NULL);
  (void) kevent(r->be_fd, &ev, 1, NULL, 0, NULL);
}

static int kqueue_wait(struct slip_ring *r, struct eng_done *out, unsigned max) {
  struct kevent evs[64];
  const int got = kevent(r->be_fd, NULL, 0, evs, 64, NULL);
  if (got < 0) return 0; /* EINTR: a harmless drain */

  struct eng_op *ready[SLIP_WAITING_MAX];
  unsigned n = 0;
  for (int e = 0; e < got; e++) {
    if ((int) evs[e].ident == r->ctl_r && evs[e].filter == EVFILT_READ) {
      slip_posix_ctl_drain(r);
      continue;
    }
    /* EV_EOF and EV_ERROR ride on the direction they were seen on - the
     * op's retry then reads the answer, the way POLLERR/POLLHUP land. */
    const short fired = filter_direction(evs[e].filter);
    for (unsigned i = 0; i < r->waiting_n && n < SLIP_WAITING_MAX; i++) {
      struct eng_op *op = r->waiting[i];
      if (op->sqe.fd == (int) evs[e].ident && (op->wait_events & fired))
        ready[n++] = op;
    }
  }
  return slip_posix_finish_ready(r, ready, n, out, max);
}

const struct eng_backend slip_backend_kqueue = {
  .name = "kqueue",
  .open_ring = kqueue_open_ring,
  .close_ring = kqueue_close_ring,
  .poke = slip_posix_poke,
  .execute = slip_posix_execute,
  .wait = kqueue_wait,
  .carried_ops = slip_posix_carried_ops,
  .arm = kqueue_arm,
  .disarm = kqueue_disarm,
};

#else
typedef int slip_backend_kqueue_is_bsd_only;
#endif
