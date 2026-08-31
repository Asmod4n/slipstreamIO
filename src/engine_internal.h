/* The engine's inside: one core owning rings, order, chains, CQ and
 * enter - and behind it one backend per platform owning how ops RUN.
 * Backends are single-threaded by construction: open_ring runs before
 * the engine thread starts, close_ring after it is joined, and
 * execute/wait only ever run ON the engine thread. poke is the one
 * cross-thread door - enter and the worker knock through it.
 *
 * Private to src/. No consumer includes this. */
#ifndef SLIPSTREAM_ENGINE_INTERNAL_H
#define SLIPSTREAM_ENGINE_INTERNAL_H

#ifdef __linux__
#include <linux/io_uring.h>
#else
/* The carried liburing's copy of the kernel header - the same file,
 * installed where the packaging says. Off Linux it is the only spelling
 * of these structs there is. */
#include <liburing/io_uring.h>
#endif

#include "thrd_compat.h"

#include <errno.h>
#include <stddef.h>

/* FreeBSD's errno.h withholds the NAME under a bare -std=c11 - the
 * value exists, __XSI_VISIBLE is just off - found by the FreeBSD VM
 * run. The engine answers the errno the platform's consumers compare
 * against, so the platform's value is spelled out where the name is
 * hidden: 60 on the BSDs and macOS (glibc, which says 62, always shows
 * the name, so this never fires there). */
#ifndef ETIME
#define ETIME 60
#endif

#define SLIP_RINGS_MAX 64
#define SLIP_WAITING_MAX 1024

/* One submitted op, copied out of the caller's SQE slot so the slot is
 * reusable the moment enter returns - the same promise the kernel makes. */
struct eng_op {
  struct io_uring_sqe sqe;
  struct eng_op *next;
  short wait_events; /* POLLIN/POLLOUT while parked (readiness backends) */
  int stalls_queue;  /* a linked op left the queue unfinished: it waits */
  void *be_source;   /* dispatch: the source; iocp: the overlapped wrapper */
};

struct slip_ring {
  int in_use;
  unsigned sq_entries, cq_entries;
  void *sq_block;
  void *cq_block;
  struct io_uring_sqe *sqes;
  size_t sq_size, cq_size, sqes_size;

  /* ---- the handoff, and the completions ---------------------------- */
  mtx_t mtx;  /* inbox, worker queue, CQ posting, backlog, blocked_done */
  cnd_t cv;   /* a completion was posted - enter's wait side */
  cnd_t wq_cv;
  struct eng_op *inbox_head, *inbox_tail; /* enter -> engine */
  struct eng_op *backlog_head, *backlog_tail; /* completions the CQ had no room for */
  struct eng_op *wq_head, *wq_tail; /* engine -> worker (regular files, POSIX) */
  struct eng_op *blocked_done; /* the op the queue stalled on, finished off-thread */
  int blocked_failed; /* that op's result was negative - the chain must know */

  /* ---- what the ENGINE THREAD owns --------------------------------- */
  struct eng_op *queue_head, *queue_tail; /* submitted, in order */
  struct eng_op *waiting[SLIP_WAITING_MAX]; /* parked (readiness backends) */
  unsigned waiting_n;
  int chain_failed; /* a linked op failed: cancel the rest of its chain */
  struct eng_op *blocking; /* a linked op is pending somewhere; the queue waits */

  const struct eng_backend *be;
  int be_fd;      /* epoll/kqueue descriptor; poll keeps none */
  void *be_state; /* dispatch: queue+semaphore+fired; iocp: the port */

  int ctl_r, ctl_w; /* the POSIX poke pipe; iocp pokes its port instead */
  thrd_t engine;
  int engine_live;
  thrd_t worker;
  int worker_live;
  int stopping;
};

/* What execute answered. */
enum eng_exec {
  EXEC_DONE,    /* res holds the completion; the core posts it */
  EXEC_PENDING, /* the backend holds the op - parked, issued, or at the worker */
};

/* One finished op out of wait. */
struct eng_done {
  struct eng_op *op;
  int res;
};

/* What a backend is: how an op runs, and how its completion comes back.
 * execute either finishes the op right there (EXEC_DONE, res set - the
 * inline NOP, the recv that had data, the refusal) or takes it
 * (EXEC_PENDING). wait blocks until something happened and fills out[]
 * with ops that FINISHED - a readiness backend retries its ready parked
 * ops itself and re-parks the spurious ones; a completion backend just
 * translates its packets. Zero from wait is a bare poke. poke may be
 * called from any thread; everything else is the engine thread's. */
struct eng_backend {
  const char *name;
  int (*open_ring)(struct slip_ring *r);
  void (*close_ring)(struct slip_ring *r);
  void (*poke)(struct slip_ring *r);
  int (*execute)(struct slip_ring *r, struct eng_op *op, int *res);
  int (*wait)(struct slip_ring *r, struct eng_done *out, unsigned max);
  /* Readiness backends only: the mirror of waiting[] membership - arm
   * on park, disarm on unpark, called by the shared POSIX machinery. A
   * backend whose execute never parks leaves them NULL. */
  int (*arm)(struct slip_ring *r, struct eng_op *op);
  void (*disarm)(struct slip_ring *r, struct eng_op *op);
};

/* The core's completion door, for whoever finishes an op off the engine
 * thread's own loop - the worker, and iocp's drain. Takes the CQ lock,
 * frees the op (or backlogs it when the CQ is full). */
void slip_engine_post(struct slip_ring *r, struct eng_op *op, int res);

/* ---- shared by the readiness backends (engine_posix.c) ---------------
 * poll, epoll, kqueue and dispatch differ ONLY in how they learn that a
 * parked descriptor came ready. Everything else - the poke pipe, the
 * DONTWAIT/poll-guarded try, parking, the worker for regular files, the
 * retry when readiness fires - is one implementation. A backend
 * supplies arm/disarm as the mirror of waiting[] membership and calls
 * finish_ready from its wait. */
#ifndef _WIN32
int slip_posix_ctl_open(struct slip_ring *r);
void slip_posix_ctl_close(struct slip_ring *r);
void slip_posix_ctl_drain(struct slip_ring *r);
void slip_posix_poke(struct slip_ring *r);
int slip_posix_execute(struct slip_ring *r, struct eng_op *op, int *res);
int slip_posix_finish_ready(struct slip_ring *r, struct eng_op **ready, unsigned ready_n,
                            struct eng_done *out, unsigned max);
#endif

extern const struct eng_backend slip_backend_poll;
#ifdef __linux__
extern const struct eng_backend slip_backend_epoll;
#endif
#if defined(__FreeBSD__) || defined(__DragonFly__) || defined(__NetBSD__) || \
    defined(__OpenBSD__) || defined(__APPLE__) || defined(SLIPSTREAM_HAVE_LIBKQUEUE)
extern const struct eng_backend slip_backend_kqueue;
#endif
#if defined(__APPLE__) || defined(SLIPSTREAM_HAVE_LIBDISPATCH)
extern const struct eng_backend slip_backend_dispatch;
#endif
#ifdef _WIN32
extern const struct eng_backend slip_backend_iocp;
#endif

#endif
