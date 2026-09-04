/* The engine's inside: one core owning rings, order, chains, CQ and
 * enter - and behind it one backend per platform owning how ops RUN.
 * Backends are single-threaded by construction: open_ring runs before
 * the engine thread starts, close_ring after it is joined, and
 * execute/wait only ever run ON the engine thread. poke is the one
 * cross-thread door - enter and the worker knock through it.
 *
 * Private to src/. No consumer includes this.
 *
 * WHERE THE BEHAVIOUR COMES FROM, before anyone writes an op here:
 * THIRD_PARTY.md, "Where this engine's behavior comes from". The short
 * of it - the ABI header and liburing's own code are MIT and may be
 * read; the kernel is GPL and may not, not even from memory of having
 * read it. What those two leave open is settled by ASKING a running
 * kernel (test/parity.c), never by remembering one. */
#ifndef SLIPSTREAM_ENGINE_INTERNAL_H
#define SLIPSTREAM_ENGINE_INTERNAL_H

/* The carried liburing's copy of the kernel header, on EVERY platform,
 * Linux included: the engine speaks the ABI of the liburing it stands
 * under, and a host's /usr/include/linux may be older than that - this
 * machine's lacked IORING_OP_BIND, measured. The packaging puts the
 * carried tree on the include path; the Makefile refuses with words
 * when there is none. */
#include <liburing/io_uring.h>

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
#define SLIP_BUFRINGS_MAX 16

/* One submitted op, copied out of the caller's SQE slot so the slot is
 * reusable the moment enter returns - the same promise the kernel makes. */
struct eng_op {
  struct io_uring_sqe sqe;
  struct eng_op *next;
  short wait_events; /* POLLIN/POLLOUT while parked (readiness backends) */
  /* Where this op sits in waiting[], so leaving the set is a swap and
   * not a search. Meaningful only while parked; waiting[slot] == op is
   * what says so, since a fresh op starts at zero. */
  unsigned wait_slot;
  int stalls_queue;  /* a linked op left the queue unfinished: it waits */
  unsigned cqe_flags; /* rides into the completion CQE (F_BUFFER and its bid) */
  void *be_source;   /* dispatch: the channel wrapper; iocp: the overlapped one */
};

/* One registered provided-buffer ring (IORING_REGISTER_PBUF_RING). The
 * entries live in the CALLER's memory at ring_addr - the tail overlays
 * bufs[0].resv and the caller advances it with a release store, exactly
 * the shared layout the kernel reads - so the engine holds a pointer and
 * its own consumed head, nothing more. */
struct slip_bufring {
  struct io_uring_buf *bufs; /* the caller's ring memory */
  unsigned entries;          /* power of two */
  unsigned short bgid;
  unsigned short head; /* engine-consumed; the caller owns the tail */
  int in_use;
};

struct slip_ring {
  int in_use;
  unsigned sq_entries, cq_entries;

  /* Ops come from HERE, not from the heap: one block carved at setup,
   * sized by what the caller asked the ring to be, and handed out
   * through a free list threaded on eng_op::next. A malloc per
   * submitted SQE and a free per completion is a per-request
   * allocation on the one path that must not have one. */
  struct eng_op *op_pool;
  unsigned op_pool_n;
  struct eng_op *op_free;

  /* The one block a ring is, and the four views into it. */
  void *block;
  size_t block_size;
  void *sq_block;
  void *cq_block;
  struct io_uring_sqe *sqes;
  size_t sq_size, cq_size, sqes_size;

  /* ---- the handoff, and the completions ---------------------------- */
  mtx_t mtx;  /* worker queue, CQ posting, backlog, blocked_done */
  cnd_t wq_cv;
  struct eng_op *backlog_head, *backlog_tail; /* completions the CQ had no room for */
  struct eng_op *wq_head, *wq_tail; /* engine -> worker (regular files, POSIX) */
  struct eng_op *blocked_done; /* the op the queue stalled on, finished off-thread */
  int blocked_failed; /* that op's result was negative - the chain must know */

  /* ---- what WHOEVER IS INSIDE ENTER owns ---------------------------
   * The submitter runs the ops, parks them and drains the backend on its
   * own thread, so these need no lock: io_uring's own single-issuer
   * shape, and the one this engine is used in. */
  struct eng_op *queue_head, *queue_tail; /* submitted, in order */
  /* Parked (readiness backends), and the two scratch arrays one wait
   * fills: the ops a backend found ready, and the completions it hands
   * back. All three are carved from the ring's own block and all three
   * are op_pool_n long, because THAT is the bound - an op that exists
   * came from the pool, so no more of them can be parked, ready or done
   * at once than the pool holds. */
  struct eng_op **waiting;
  unsigned waiting_n;
  struct eng_op **ready;
  struct eng_done *done;
  int chain_failed; /* a linked op failed: cancel the rest of its chain */
  struct eng_op *blocking; /* a linked op is pending somewhere; the queue waits */

  /* ---- registered resources ----------------------------------------
   * Created by the register path before traffic and torn down after it
   * (webmachine's shape, and SINGLE_ISSUER's); the ops that mutate
   * entries at runtime - a direct accept installing, close_direct
   * clearing - run on the engine thread only. */
  int *fixed;       /* the fixed file table: real descriptors, -1 empty */
  unsigned fixed_n;
  unsigned alloc_off, alloc_len; /* FILE_ALLOC_RANGE; the whole table until set */
  unsigned alloc_hint;           /* where the next ALLOC scan starts */
  struct slip_bufring bufrings[SLIP_BUFRINGS_MAX];

  const struct eng_backend *be;
  int be_fd;      /* epoll/kqueue descriptor; select keeps none */
  void *be_state; /* dispatch: queue+semaphore+done; iocp: the port */

  /* The poke: the WORKER's door to a submitter blocked in the backend's
   * wait. Nothing else knocks - a submit runs on the submitter's own
   * thread and has nobody to wake. */
  int ctl_r, ctl_w;
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
  /* Blocks until something happened, at most timeout_ms (-1 forever, 0
   * a look). Runs on the CALLER's thread, inside enter - there is no
   * engine thread; see slipstream_engine.c. */
  int (*wait)(struct slip_ring *r, struct eng_done *out, unsigned max, int timeout_ms);
  /* Readiness backends only: the mirror of waiting[] membership - arm
   * on park, disarm on unpark, called by the shared POSIX machinery. A
   * backend whose execute never parks leaves them NULL. */
  int (*arm)(struct slip_ring *r, struct eng_op *op);
  void (*disarm)(struct slip_ring *r, struct eng_op *op);
  /* An op with no readiness to wait for - a regular file, a connect, an
   * openat - and therefore one that has to run somewhere it may block.
   * A backend that brings its own place for that (dispatch hands it to
   * GCD, which is the platform's own work queue) says so here; the rest
   * leave it NULL and the shared worker thread runs it. Whoever takes
   * it owes the op a completion through the backend's own wait. */
  void (*submit_blocking)(struct slip_ring *r, struct eng_op *op);
  /* A descriptor is gone - closed by an op, or handed back by the fixed
   * table. A backend that remembers descriptors between ops has to
   * forget this one HERE: the number comes back, and a table that still
   * claims it is registered would skip the registration the new one
   * needs and park it forever. Backends that remember nothing leave
   * this NULL. */
  void (*forget)(struct slip_ring *r, int fd);
  /* The opcodes this backend actually runs, 255-terminated (every real
   * opcode is below IORING_OP_LAST) - what REGISTER_PROBE reports, so
   * the probe tells the truth per backend instead of a flattering
   * lie. */
  const unsigned char *carried_ops;
};

/* The core's completion door, for whoever finishes an op off the engine
 * thread's own loop - the worker, and iocp's drain. Takes the CQ lock,
 * frees the op (or backlogs it when the CQ is full). */
void slip_engine_post(struct slip_ring *r, struct eng_op *op, int res);

/* A CQE that is NOT an op's completion - a multishot emission. The op
 * stays alive and parked; only the CQE goes out. Engine thread only. */
void slip_engine_emit(struct slip_ring *r, __u64 user_data, int res, unsigned flags);

/* The fixed file table, owned by the core beside the register path.
 * lookup: slot -> real descriptor, -EBADF outside the table or empty.
 * install: file_index as the SQE carries it - slot+1, or
 * IORING_FILE_INDEX_ALLOC to pick from the alloc range - returns the
 * slot, -ENFILE when the range is full, -EBADF outside the table. An
 * occupied explicit slot is replaced and the old descriptor closed, the
 * kernel's update semantics. clear: empties the slot WITHOUT closing -
 * close_direct closes, a replace already closed. Engine thread only. */
int slip_fixed_lookup(struct slip_ring *r, int slot);
int slip_fixed_install(struct slip_ring *r, __u32 file_index, int realfd);
int slip_fixed_clear(struct slip_ring *r, unsigned slot);

/* The registered buffer ring of one group, NULL when none is. */
struct slip_bufring *slip_bufring_of(struct slip_ring *r, unsigned short bgid);

/* Closing a real descriptor is the one OS call the fixed table needs and
 * the core does not hold - each family supplies its platform's spelling
 * (close on POSIX, closesocket/CloseHandle territory on Windows). */
void slip_native_fd_close(int fd);

/* ---- the two families -------------------------------------------------
 * READINESS (select, epoll, kqueue): the OS says "that descriptor came
 * ready" and the shared machinery in engine_posix.c does everything
 * else - the poke pipe, the DONTWAIT-guarded try, parking mirrored via
 * arm/disarm, the retry, the worker for regular files. A backend of
 * this family is only its wakeup.
 * COMPLETION (iocp): the OS RUNS the op and reports the outcome - the
 * same shape io_uring itself has. Nothing parks; execute issues, wait
 * translates results, arm/disarm stay NULL.
 * dispatch is the readiness family wearing GCD: a dispatch source says
 * "ready" and the SAME shared machinery runs the op, so macOS answers
 * what every other backend answers. What has no readiness goes to GCD's
 * own queues through submit_blocking - a dispatch_io channel for a
 * positioned file read, the global concurrent queue for the rest - and
 * never to a thread of this engine's. */
#ifndef _WIN32
/* What one attempt at an op answered: finished, would block (park it on
 * wait_events), or has no readiness to wait for and needs running
 * somewhere that may block. The completion family reaches for this
 * wherever its own machinery has no spelling for an op - a socket() is
 * a socket() whichever backend is underneath. */
enum eng_verdict { ENG_RAN, ENG_PARK, ENG_FILE };
enum eng_verdict slip_posix_try(struct slip_ring *r, struct eng_op *op, int *res_out);

int slip_posix_ctl_open(struct slip_ring *r);
void slip_posix_ctl_close(struct slip_ring *r);
void slip_posix_ctl_drain(struct slip_ring *r);
void slip_posix_poke(struct slip_ring *r);
int slip_posix_execute(struct slip_ring *r, struct eng_op *op, int *res);
int slip_posix_finish_ready(struct slip_ring *r, struct eng_op **ready, unsigned ready_n,
                            struct eng_done *out, unsigned max);
/* Hand one op to the blocking worker directly - for a completion
 * backend whose API cannot express the op (dispatch_io has no recv
 * flags) but whose platform can still run it correctly off the engine
 * thread. */
void slip_posix_hand_to_worker(struct slip_ring *r, struct eng_op *op);
/* The blocking spelling of one op - read/write on a regular file,
 * connect, openat - run to the end, errno already turned into the
 * negative the CQE carries. The shared worker calls it; so does a
 * backend that brought its own place to block. */
int slip_posix_run_blocking(const struct io_uring_sqe *s);
/* The socket commands (IORING_OP_URING_CMD, SOCKET_URING_OP_*): plain
 * syscalls, so the completion family answers them from here too. */
int slip_posix_cmd_sock(const struct io_uring_sqe *s);
/* One list for the whole readiness family - they all run the same
 * run_one. */
extern const unsigned char slip_posix_carried_ops[];
#endif

/* The floor: every platform has select, so this one always exists. */
extern const struct eng_backend slip_backend_select;
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
