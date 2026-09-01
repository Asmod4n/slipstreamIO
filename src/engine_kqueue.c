/* The BSD backend: kqueue holds the parked set. A knote is per
 * descriptor AND filter, so a recv and a send parked on one socket are
 * two registrations with no merging.
 *
 * KNOTES STAY, and a delivery hands over its own waiters - the same two
 * things the epoll backend learned from measuring, and for the same
 * reasons. kqueue is level triggered unless EV_CLEAR says otherwise, so
 * a knote whose op has finished reports nothing while the socket is
 * drained; keeping it saves an EV_DELETE now and an EV_ADD for the next
 * request. And the ops wait in a table beside the knote, per descriptor
 * and side, so an event is answered by reading them out instead of
 * walking every parked op to find out whose descriptor it was.
 *
 * Also compiles and runs against libkqueue on Linux - the kqueue API
 * over epoll, the same packaging move this project makes for liburing.h
 * - which is how it is proven without a BSD; test/freebsd_vm.sh is the
 * native proof. */
#if defined(__FreeBSD__) || defined(__DragonFly__) || defined(__NetBSD__) || \
    defined(__OpenBSD__) || defined(__APPLE__) || defined(SLIPSTREAM_HAVE_LIBKQUEUE)

#include "engine_internal.h"

#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

/* POLLPRI has no filter of its own on every BSD, so it rides the read
 * side: out-of-band arriving wakes EVFILT_READ, and the retry then
 * reads what actually came. */
static int kq_filter_of(short wait_events) {
  return (wait_events & POLLOUT) ? EVFILT_WRITE : EVFILT_READ;
}

/* Per descriptor: which knotes are registered, and who waits behind
 * them. A second op on the same side chains through the op's own next
 * pointer, which a parked op is not otherwise using. */
struct kq_fd {
  int armed_read, armed_write;
  struct eng_op *in, *out;
};

struct kq_state {
  struct kq_fd *fds;
  unsigned n;
};

static struct kq_fd *kq_slot(struct slip_ring *r, int fd) {
  struct kq_state *st = r->be_state;
  if (fd < 0) return NULL;
  if ((unsigned) fd >= st->n) {
    unsigned want = st->n ? st->n : 64;
    while (want <= (unsigned) fd) want *= 2;
    struct kq_fd *grown = realloc(st->fds, (size_t) want * sizeof(*grown));
    if (grown == NULL) return NULL;
    memset(grown + st->n, 0, (size_t) (want - st->n) * sizeof(*grown));
    st->fds = grown;
    st->n = want;
  }
  return &st->fds[fd];
}

static int kqueue_open_ring(struct slip_ring *r) {
  if (slip_posix_ctl_open(r) != 0) return -1;
  struct kq_state *st = calloc(1, sizeof(*st));
  if (st == NULL) {
    slip_posix_ctl_close(r);
    return -1;
  }
  r->be_state = st;
  r->be_fd = kqueue();
  if (r->be_fd < 0) {
    free(st);
    r->be_state = NULL;
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
  struct kq_state *st = r->be_state;
  if (st != NULL) {
    free(st->fds);
    free(st);
    r->be_state = NULL;
  }
  slip_posix_ctl_close(r);
}

static int kqueue_arm(struct slip_ring *r, struct eng_op *op) {
  struct kq_fd *slot = kq_slot(r, op->sqe.fd);
  if (slot == NULL) return -1;
  const int writing = (op->wait_events & POLLOUT) != 0;
  struct eng_op **side = writing ? &slot->out : &slot->in;
  int *armed = writing ? &slot->armed_write : &slot->armed_read;
  /* Only trustworthy while someone is parked on this descriptor - see
   * the same rule in the epoll backend. An idle entry may be talking
   * about a file that closed and a number that came back, and kqueue
   * has no error to answer that with: a skipped EV_ADD is silent, and
   * the op waits forever. parity hung on exactly that. */
  const int idle = slot->in == NULL && slot->out == NULL;
  if (idle) {
    slot->armed_read = 0;
    slot->armed_write = 0;
  }
  op->next = *side;
  *side = op;
  if (*armed) return 0; /* the knote is there, and vouched for */

  struct kevent ev;
  EV_SET(&ev, (uintptr_t) op->sqe.fd, kq_filter_of(op->wait_events), EV_ADD, 0, 0, NULL);
  if (kevent(r->be_fd, &ev, 1, NULL, 0, NULL) == 0) {
    *armed = 1;
    return 0;
  }
  *side = op->next;
  op->next = NULL;
  return -1;
}

/* The op leaves its side; the KNOTE stays, for the next request on
 * this descriptor to find already there. */
static void kqueue_disarm(struct slip_ring *r, struct eng_op *op) {
  struct kq_fd *slot = kq_slot(r, op->sqe.fd);
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

static int kqueue_wait(struct slip_ring *r, struct eng_done *out, unsigned max,
                       int timeout_ms) {
  struct kevent evs[64];
  struct timespec ts;
  if (timeout_ms >= 0) {
    ts.tv_sec = timeout_ms / 1000;
    ts.tv_nsec = (long) (timeout_ms % 1000) * 1000000L;
  }
  const int got = kevent(r->be_fd, NULL, 0, evs, 64, timeout_ms >= 0 ? &ts : NULL);
  if (got < 0) return 0; /* EINTR: a harmless drain */

  struct eng_op **ready = r->ready;
  unsigned n = 0;
  for (int e = 0; e < got; e++) {
    if ((int) evs[e].ident == r->ctl_r && evs[e].filter == EVFILT_READ) {
      slip_posix_ctl_drain(r);
      continue;
    }
    /* NO SEARCH: the descriptor that woke hands over its own waiters.
     * EV_EOF and EV_ERROR ride on the direction they were seen on - the
     * op's retry then reads the answer, the way POLLERR/POLLHUP land. */
    struct kq_fd *slot = kq_slot(r, (int) evs[e].ident);
    if (slot == NULL) continue;
    const int writing = evs[e].filter == EVFILT_WRITE;
    struct eng_op **side = writing ? &slot->out : &slot->in;
    const unsigned before = n;
    for (struct eng_op *op = *side; op != NULL && n < r->op_pool_n; op = op->next)
      ready[n++] = op;
    /* Woken with nobody behind it: a knote that outlived its ops. */
    if (n == before) {
      struct kevent del;
      EV_SET(&del, evs[e].ident, evs[e].filter, EV_DELETE, 0, 0, NULL);
      (void) kevent(r->be_fd, &del, 1, NULL, 0, NULL);
      if (writing) slot->armed_write = 0;
      else slot->armed_read = 0;
    }
  }
  return slip_posix_finish_ready(r, ready, n, out, max);
}

/* The descriptor is closing: kqueue drops its knotes on the last close,
 * so only this table has to be told - and it MUST be, because nothing
 * else would notice. epoll survives a stale entry because EPOLL_CTL_MOD
 * answers -ENOENT; a skipped EV_ADD answers nothing at all, and the op
 * that needed it waits forever. Found exactly that way, by parity
 * hanging on the tenth scenario. */
static void kqueue_forget(struct slip_ring *r, int fd) {
  struct kq_fd *slot = kq_slot(r, fd);
  if (slot == NULL) return;
  slot->armed_read = 0;
  slot->armed_write = 0;
  slot->in = NULL;
  slot->out = NULL;
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
  .forget = kqueue_forget,
};

#else
typedef int slip_backend_kqueue_is_bsd_only;
#endif
