/* slipstreamIO: the io_uring submission/completion API, run on
 * select(2) plus one engine thread.
 *
 * This file is named for the API it implements, because that is the
 * only name anyone ever writes. It does NOT sit on an include path by
 * itself - mrbgem.rake copies it into include/ exactly when this host
 * has no other liburing.h, and leaves it here otherwise.
 *
 * IT IS C, and for the same reason it carries that name: real
 * liburing.h is C, consumable from C and from C++, and a stand-in that
 * is only consumable from one of them is not a stand-in. So: C11,
 * static inline throughout, plain structs, <threads.h> for the engine,
 * <stdatomic.h> for the completion ring's cursors - with a C++ arm
 * around the two that C++ spells differently (<atomic>), and nothing
 * else in here that a C compiler cannot read.
 *
 * WHAT THIS IS: an implementation of that API's shape - submission
 * queue entries in, completion queue entries out - for machines where
 * the real thing is not available. It knows NOTHING about the library
 * it stands in for: it never probes for it, never includes it, never
 * decides anything. It is one implementation, and choosing it is
 * somebody else's job.
 *
 * WHERE THE CHOICE IS MADE, and deliberately not here: at the packaging
 * layer, the way libkqueue is chosen on Linux. A consumer writes
 * `#include <liburing.h>` and calls the functions; whoever assembles
 * the build decides which implementation that resolves to - by
 * installing this header under that name, or by not installing it.
 * Source code never asks, so there is no __has_include, no define to
 * keep in sync, no template parameter and no runtime branch anywhere
 * above this file. The binary also stays statically linkable: there is
 * nothing to dlopen, which matters because a static dlopen fails on
 * glibc and musl alike.
 *
 * THE TWO HALVES, and which one is whose. Real io_uring is a userspace
 * half (submit, walk completions) and a KERNEL half (run the ops, post
 * the completions, make one fd readable when there are any). This file
 * implements both, because both are the API. The second half is threads
 * of this file's own, born in io_uring_queue_init* and joined in
 * io_uring_queue_exit, with no API surface at all - no run function, no
 * callback, no handle, nothing to configure - and it has the same two
 * parts the original does:
 *
 *   THE ENGINE, one per struct io_uring: owns the select loop, runs
 *   every READINESS operation, posts every completion, and is the only
 *   thread that makes ring_fd readable. It never blocks on work.
 *
 *   THE WORK THREADS, io_uring's io-wq by another name: a small pool
 *   that runs the BLOCKING operations - openat, read, statx, closing a
 *   file - so that the engine does not. They never post a completion;
 *   they hand results back to the engine, which is what keeps the
 *   completion ring single-producer.
 *
 * The DRIVER loop is still entirely the embedder's: this file never
 * calls user code and never owns the caller's flow of control.
 *
 * WHAT THAT BUYS, and what nothing else could:
 *   - ring_fd is a real, pollable descriptor, readable exactly when
 *     completions are waiting, so a ring hangs into a foreign event
 *     loop the way liburing's does;
 *   - completions arrive while the caller is COMPUTING, not only while
 *     it is inside one of these functions;
 *   - FILE IO becomes possible at all. select(2) says "ready" about a
 *     regular file always and immediately, so a file read has to be
 *     run by somebody, blocking, and the one thing it must not block is
 *     the caller. A work thread blocks on it instead: openat, read,
 *     statx and close on file descriptors are ordinary ops here.
 * Without a thread of its own, none of the three is implementable on
 * select(2). That was a structural gap, not a missing feature.
 *
 * DESIGN GOAL, explicit: this is CORRECT, not fast. Laziness is the
 * declared motive. Nobody optimizes it later.
 *   - every socket operation is readiness + a classic syscall
 *   - submission is pure handoff: io_uring_submit moves the prepped
 *     SQEs to the engine, in order, and returns. Nothing runs on the
 *     caller's thread, so a completion is never guaranteed to be there
 *     the instant submit returns - wait for it, exactly as against a
 *     real ring
 *   - recv bundles do not exist; one buffer per completion, so the
 *     dense-fill contract holds trivially
 *   - the pool is small and fixed (four), and there is no knob to make
 *     it bigger. It exists to keep the engine free, not to parallelise
 *     a disk.
 *   - capacity is capped below FD_SETSIZE by whoever sets the limit:
 *     here a connection IS a process fd. That ceiling is PUBLISHED, as
 *     IO_URING_FD_CEILING below, so a consumer reads the property
 *     instead of guessing it from this implementation's name
 *
 * WHY select IS THE BASELINE, and not poll: it is the one readiness
 * primitive that exists everywhere AND has been debugged everywhere -
 * macOS' poll is permanently broken on several fd types, and WSAPoll
 * does not report failed connections (acknowledged by Microsoft, never
 * fixed). kqueue and IOCP are not rejected here, they are the PLANNED
 * native implementations (TASKS.md): one portable baseline that works
 * everywhere, native engines per platform on top of it. The operations
 * below are still Linux syscalls; the SHAPE is what is portable, and no
 * line here pretends the port has been done.
 */
#ifndef SLIPSTREAM_IO_H
#define SLIPSTREAM_IO_H

/* The ops below CALL things glibc hides behind _GNU_SOURCE: accept4,
 * SOCK_CLOEXEC, MSG_NOSIGNAL, statx. Real liburing.h never needs this,
 * because it never calls anything - it fills SQEs and the kernel runs
 * them. This file is the implementation, so it needs the declarations.
 * g++ defines _GNU_SOURCE for you; `gcc -std=c11` does not, which is
 * exactly the difference this line exists for. It has to come before
 * the first libc include to take effect, so if a consumer got there
 * first, the check further down says so in one sentence instead of
 * fifty. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

/* The name of this implementation, and NOTHING that follows from it.
 * It exists so a build can SAY which implementation answered - the
 * startup banner that reports "correct, not fast" is the whole use
 * case. No limit and no behaviour may be derived from it: a name is not
 * a property, and every implementation of this API that comes later
 * would inherit whatever was hung on this one's name. */
#define SLIPSTREAM_IO 1

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <threads.h>
#include <time.h>
#include <unistd.h>

#if defined(__GLIBC__) && !defined(__USE_GNU)
#error "<liburing.h> (slipstreamIO): a libc header was included before this one \
without _GNU_SOURCE, so accept4/MSG_NOSIGNAL/statx are not declared. \
Build with -D_GNU_SOURCE (g++ already does), or include this header first."
#endif

/* The completion ring's cursors are the one place two threads meet, so
 * they are atomics and nothing else is. C spells that <stdatomic.h>,
 * C++ spells it <atomic>; the four spellings that differ are wrapped
 * here so the code below is written once. */
#ifdef __cplusplus
#include <atomic>
#define SLIPSTREAM_ATOMIC(T) std::atomic<T>
#define slipstream_mo_relaxed std::memory_order_relaxed
#define slipstream_mo_acquire std::memory_order_acquire
#define slipstream_mo_release std::memory_order_release
#define slipstream_load(p, mo) std::atomic_load_explicit(p, mo)
#define slipstream_store(p, v, mo) std::atomic_store_explicit(p, v, mo)
#else
#include <stdatomic.h>
#define SLIPSTREAM_ATOMIC(T) _Atomic T
#define slipstream_mo_relaxed memory_order_relaxed
#define slipstream_mo_acquire memory_order_acquire
#define slipstream_mo_release memory_order_release
#define slipstream_load(p, mo) atomic_load_explicit(p, mo)
#define slipstream_store(p, v, mo) atomic_store_explicit(p, v, mo)
#endif

/* __kernel_timespec: liburing gets this from <linux/time_types.h>
 * (through its own compat.h), so use the real thing wherever it is
 * available - not just on Linux, in case some other host ships it too.
 * Guarded with the kernel header's own include guard, so this fallback
 * steps aside the moment the real header is reachable, and a later
 * include of it (directly, or pulled in by something else) never
 * collides with a struct already defined here. Where neither happens -
 * the case this exists for - the shape is the kernel's own: two 64-bit
 * fields, nothing else, so a consumer that reads tv_sec/tv_nsec sees
 * the same layout it would from the real header. */
#if __has_include(<linux/time_types.h>)
#include <linux/time_types.h>
#elif !defined(_LINUX_TIME_TYPES_H)
#define _LINUX_TIME_TYPES_H
struct __kernel_timespec {
  int64_t tv_sec;
  int64_t tv_nsec;
};
#endif

/* statx: liburing declares the pointer type and never defines it, and
 * neither do we - <sys/stat.h> above brings the real one where the host
 * has it, and this line only makes the prototype below legal where it
 * does not. The op itself answers -EOPNOTSUPP there. */
struct statx;

/* ---- the property a consumer may branch on --------------------------
 *
 * IO_URING_FD_CEILING: every descriptor handed to these functions must
 * be strictly below this number. It is stated because it is TRUE here
 * and for no other reason - select(2) addresses FD_SETSIZE descriptors
 * and nothing above, and a consumer whose connections are process fds
 * has to keep its own rlimit under the same roof or hand this API an
 * fd it cannot put in an fd_set.
 *
 * ABSENCE is the other half of the contract, and the important half: an
 * implementation with no ceiling of its own defines nothing, and real
 * liburing never will. A consumer that finds no ceiling has been told
 * there is none - its rlimits are the only bound. So the question to
 * ask is `#ifdef IO_URING_FD_CEILING`, never `#ifdef SLIPSTREAM_IO`:
 * the second asks WHO answered and gets select's ceiling handed to
 * every future answer, including IOCP (no fd_set at all) and a macOS
 * build with _DARWIN_UNLIMITED_SELECT (heap fd_sets, a different
 * number). Written down while there is exactly one consumer, because
 * that is when it is cheap.
 *
 * It sits below the includes because FD_SETSIZE is <sys/select.h>'s to
 * define, and this file only reports it. */
#define IO_URING_FD_CEILING FD_SETSIZE

/* ---- the ABI names, as far as this tree uses them -------------------
 *
 * These mirror liburing's spelling, NOT the kernel's layout: in a build
 * without liburing nothing else in the process shares these structs, so
 * they only have to carry what the prep_* writers below record and what
 * the engine reads back. Field names match liburing's so that ring.hpp's
 * direct writes (s->flags |= IOSQE_FIXED_FILE) compile unchanged. */

enum {
  IORING_OP_NOP = 0,
  IORING_OP_ACCEPT,
  IORING_OP_BIND,
  IORING_OP_CLOSE,
  IORING_OP_LISTEN,
  IORING_OP_OPENAT,
  IORING_OP_ASYNC_CANCEL,
  IORING_OP_POLL_ADD,
  IORING_OP_POLL_REMOVE,
  IORING_OP_READ,
  IORING_OP_RECV,
  IORING_OP_SEND,
  IORING_OP_SENDMSG,
  IORING_OP_SHUTDOWN,
  IORING_OP_SOCKET,
  IORING_OP_STATX,
  IORING_OP_UNLINKAT,
  IORING_OP_URING_CMD
};

enum {
  IOSQE_FIXED_FILE = 1u << 0,
  IOSQE_IO_LINK = 1u << 2,
  IOSQE_BUFFER_SELECT = 1u << 3,
  IOSQE_CQE_SKIP_SUCCESS = 1u << 4
};

enum {
  IORING_CQE_F_BUFFER = 1u << 0,
  IORING_CQE_F_MORE = 1u << 1
};

/* RFC-free, this is liburing's own: a poll that STAYS armed until it is
 * removed, reporting every readiness with IORING_CQE_F_MORE set. It is
 * what a Watcher is - built once, running until told to stop - so it is
 * the one poll form this file cares about most. */
enum {
  IORING_POLL_ADD_MULTI = 1u << 0
};

/* Cancel by DESCRIPTOR, and take everything armed on it. One way to
 * cancel is the whole point: an op family that needs its own remove is
 * a family that has to be remembered separately, and a caller holding a
 * descriptor already knows the only thing it needs to know. */
enum {
  IORING_ASYNC_CANCEL_ALL = 1u << 0,
  IORING_ASYNC_CANCEL_FD = 1u << 1
};

/* An update names the poll to change by its OLD user_data, and says
 * which of the two things it is changing. Both may be set at once. */
enum {
  IORING_POLL_UPDATE_EVENTS = 1u << 1,
  IORING_POLL_UPDATE_USER_DATA = 1u << 2
};
#define IORING_CQE_BUFFER_SHIFT 16
#define IORING_MAX_FIXED_FILES (1u << 20)
#define IORING_RECVSEND_BUNDLE (1u << 4)

/* Setup flags the Ring asks for. They describe a submission-queue
 * discipline this implementation has by construction - ONE submitter,
 * and every op run by the one engine thread in submission order - so
 * they are accepted and mean nothing here. SINGLE_ISSUER is not merely
 * tolerated: it is the contract this file is written against. The
 * submission side (get_sqe, the prep_* writers, submit, registration,
 * and the walk over completions) belongs to ONE caller thread; the
 * engine is the only other party, and it never touches that side. */
enum {
  IORING_SETUP_SINGLE_ISSUER = 1u << 12,
  IORING_SETUP_DEFER_TASKRUN = 1u << 13,
  IORING_SETUP_COOP_TASKRUN = 1u << 8
};
/* Deliberately NOT set in io_uring::features: recv bundles do not exist
 * here, so the Ring reads bundles_ = false and takes one buffer per
 * completion. */
#define IORING_FEAT_RECVSEND_BUNDLE (1u << 14)

/* The socket commands, with the KERNEL's own numbers (linux/io_uring.h)
 * so a reader recognises them - and each guarded, so a real liburing
 * always wins. SETSOCKOPT used to be spelled 1 here, which was fine
 * while it was alone and wrong the moment a second value exists: 1 is
 * SIOCOUTQ. */
#ifndef SOCKET_URING_OP_SIOCINQ
#define SOCKET_URING_OP_SIOCINQ 0
#endif
#ifndef SOCKET_URING_OP_SIOCOUTQ
#define SOCKET_URING_OP_SIOCOUTQ 1
#endif
#ifndef SOCKET_URING_OP_GETSOCKOPT
#define SOCKET_URING_OP_GETSOCKOPT 2
#endif
#ifndef SOCKET_URING_OP_SETSOCKOPT
#define SOCKET_URING_OP_SETSOCKOPT 3
#endif
#ifndef SOCKET_URING_OP_GETSOCKNAME
#define SOCKET_URING_OP_GETSOCKNAME 4
#endif

struct io_uring_sqe {
  uint8_t opcode;
  uint8_t flags;
  uint16_t ioprio;
  int32_t fd;
  uint64_t off;
  uint64_t addr;
  uint32_t len;
  uint32_t msg_flags;
  uint32_t open_flags;
  uint32_t statx_flags;
  uint32_t unlink_flags;
  uint32_t file_index;
  uint16_t buf_group;
  uint32_t cmd_op;
  uint32_t level;
  uint32_t optname;
  uint64_t optval;
  uint32_t optlen;
  /* The poll mask lives here and NOT in len, because len carries the
   * poll flags - that is how liburing spells it, and a header that
   * claims its API has to spell it the same way. */
  uint32_t poll32_events;
  uint64_t user_data;
};

struct io_uring_cqe {
  uint64_t user_data;
  int32_t res;
  uint32_t flags;
};

/* The provided-buffer ring, and the SECOND single-producer/single-
 * consumer ring in this file: the caller fills entries and publishes
 * them by advancing tail, the engine consumes them in ring order and
 * advances head. Exactly the kernel's split, so the cursors ARE the
 * synchronisation and nothing else is needed - a release on tail hands
 * the entry contents over, an acquire on tail picks them up. */
struct io_uring_buf {
  void *addr;
  unsigned len;
  uint16_t bid;
};
struct io_uring_buf_ring {
  struct io_uring_buf *bufs;
  uint32_t mask; /* written once at setup, read-only afterwards */
  SLIPSTREAM_ATOMIC(uint32_t) head; /* the engine consumes here */
  SLIPSTREAM_ATOMIC(uint32_t) tail; /* the caller provides here */
};

/* ---- the two growable queues the engine keeps -----------------------
 *
 * C, so they are spelled out: a submission queue that only ever appends
 * and clears, and a completion FIFO for the overflow backlog. Both are
 * touched by exactly one thread at a time, which is the reason they can
 * be this plain. */
struct slipstream_sqeq {
  struct io_uring_sqe *v;
  unsigned n, cap;
};
struct slipstream_cqq {
  struct io_uring_cqe *v;
  unsigned head, n, cap;
};

/* One blocking operation, on its way to a work thread and back. The
 * worker gets a COPY of the SQE with its descriptor already resolved,
 * so it never reads the engine's direct table, and it writes exactly
 * one field: res. It does not touch the completion ring - see the pool
 * note below. */
struct slipstream_work {
  struct io_uring_sqe sqe;
  int res;
  int linked; /* was IOSQE_IO_LINK set: the chain is waiting on this */
  struct slipstream_work *next;
};

/* An errand for the engine. Registration and buffer-ring setup mutate
 * state the engine reads while it is deciding what to select on, so
 * they do not run on the caller's thread at all: they are handed over,
 * executed between two select passes, and waited for. That is what
 * removes the descriptor race the inline version had - nothing can
 * close or re-point an fd while the engine holds it in an fd_set.
 * Synchronous to the caller, so one slot is enough under the
 * single-issuer contract. */
enum {
  SLIPSTREAM_ERRAND_FILES_SPARSE = 1,
  SLIPSTREAM_ERRAND_ALLOC_RANGE,
  SLIPSTREAM_ERRAND_FILES_UPDATE,
  SLIPSTREAM_ERRAND_SETUP_BUFRING
};
struct slipstream_errand {
  int kind;
  unsigned a, b;
  int *fds;
  int rc;
  int done;
  cnd_t cv;
};

/* THE WORK THREADS - the io-wq half of the model.
 *
 * The engine owns readiness and must never block: select tells it when a
 * socket is ready and every socket op it runs is one non-blocking
 * syscall. A file has no readiness - select calls every regular file
 * ready, always - so openat, read, statx and the close of a file
 * descriptor are simply blocking work, and blocking work goes to a
 * pool, exactly as real io_uring hands it to io-wq.
 *
 * FOUR, and why: this pool exists to keep the ENGINE free, not to
 * parallelise a disk. io-wq's own bounded pool defaults to the number of
 * CPUs with a floor of four, and four is the floor for the same reason
 * here - enough that a couple of slow reads cannot starve the rest,
 * small enough to stay a fixed, spawn-on-demand array. There is no knob:
 * IORING_REGISTER_IOWQ_MAX_WORKERS is deliberately not implemented,
 * because nothing has asked for it.
 *
 * Workers are spawned lazily - the first blocking op pays for the first
 * one - and park on a condition variable when there is nothing to do. A
 * ring that never touches a file never starts a thread beyond the
 * engine. */
#define SLIPSTREAM_WORKERS_MAX 4u

#define SLIPSTREAM_SQ_DEPTH 1024u
/* The completion ring is a FIXED buffer with free-running cursors, like
 * the kernel's - a power of two, so the mask is an AND. Overflow drops
 * nothing: it goes to an engine-side backlog and is pushed in as the
 * caller advances, which is the real ring's semantics too. */
#define SLIPSTREAM_CQ_DEPTH 4096u
#define SLIPSTREAM_CQ_MASK (SLIPSTREAM_CQ_DEPTH - 1u)

struct io_uring {
  unsigned features; /* no IORING_FEAT_* - bundles must read false */

  /* ---- what the CALLER owns ----------------------------------------
   * Written and read on the submitting thread only. The engine never
   * looks at sq, and never moves cq_head. */
  struct io_uring_sqe *sq; /* SLIPSTREAM_SQ_DEPTH entries, allocated once
                            * at init and never moved, so an SQE pointer
                            * a caller is still holding stays valid until
                            * it submits */
  unsigned sq_n;

  /* ---- the completion ring, and its two cursors ---------------------
   * cq_head is the caller's (io_uring_cq_advance moves it, and nothing
   * else), cq_tail is the engine's. io_uring_for_each_cqe walks
   * [head, tail) with no lock at all, because that interval is finished
   * work the engine will not touch again; the engine only ever writes
   * slots at or after tail, and only up to head + SLIPSTREAM_CQ_DEPTH. */
  struct io_uring_cqe *cqes;
  SLIPSTREAM_ATOMIC(unsigned) cq_head;
  SLIPSTREAM_ATOMIC(unsigned) cq_tail;

  /* How many armed multishot polls are QUIET - each holding a completion
   * the caller has not taken yet. Nonzero is the only reason a caller's
   * advance has to wake the engine, so it is read on that path and
   * nowhere else. See slipstream_poll_voice below for why they go quiet
   * at all. */
  SLIPSTREAM_ATOMIC(unsigned) poll_quiet;

  /* ring_fd: the read end of the COMPLETION pipe, and a real pollable
   * descriptor - the property real liburing's ring_fd has, and the whole
   * reason the engine thread exists. THE INVARIANT, stated once:
   *
   *   ring_fd is readable if and only if there are completions the
   *   caller has not consumed.
   *
   * Held up by exactly two critical sections, both under pipe_mtx, and
   * both re-checking the ring INSIDE the lock, which is what makes them
   * safe against each other:
   *   ENGINE, after publishing completions: if no byte is outstanding
   *     and head != tail, write one byte and mark it outstanding.
   *   CALLER, at the end of io_uring_cq_advance: if a byte is
   *     outstanding and head == tail, read the pipe empty and clear the
   *     mark.
   * The re-check is the point. If the caller drains first, the engine
   * then finds head != tail and re-arms; if the engine arms first, the
   * caller finds head != tail and leaves the byte alone. Neither order
   * can end with completions pending and no byte - a lost wakeup is a
   * hung embedder, so this is the one place in the file written for the
   * race and not for the common case. A byte that outlives the last
   * completion by a moment costs one spurious wakeup and nothing else,
   * because every waiter re-reads the cursors after waking. */
  int ring_fd;
  int cq_w;

  /* The CONTROL pipe, the other direction: the caller pokes it to wake
   * the engine out of select - new submissions, an errand, shutdown, or
   * room made in a full completion ring. One byte means "look again"; a
   * write that fails because the pipe is full is not an error, since a
   * wakeup is already queued. */
  int ctl_r, ctl_w;

  SLIPSTREAM_ATOMIC(int) cq_armed;    /* a byte is outstanding in the completion pipe */
  SLIPSTREAM_ATOMIC(int) cq_overflow; /* the engine is holding a backlog */
  /* True only while the engine sits in select with NOTHING left to do:
   * no submission, no errand, nothing armed, no backlog. An untimed
   * wait reads this to answer "could anything still complete?" and
   * returns instead of blocking forever - the same answer the inline
   * implementation gave by finding its waiting set empty. */
  SLIPSTREAM_ATOMIC(int) engine_idle;
  mtx_t pipe_mtx;

  /* ---- the handoff -------------------------------------------------- */
  mtx_t mtx;
  struct slipstream_sqeq pending; /* submitted, not yet taken */
  struct slipstream_errand *errand;
  int stopping;
  thrd_t engine;
  int engine_live;

  /* ---- what the ENGINE owns -----------------------------------------
   * Reached only from the engine thread, or before it is started and
   * after it is joined. Registration, buffer-ring setup and close all
   * run as errands ON the engine for exactly this reason. */
  int *files; /* slot -> fd; the direct table, spelled out */
  unsigned files_n;
  uint32_t *free_slots; /* the alloc range's free list */
  unsigned free_n, free_cap;
  struct slipstream_sqeq waiting; /* armed ops awaiting readiness */
  struct slipstream_cqq backlog;  /* completions the ring had no room for */
  struct io_uring_buf_ring bufring;

  /* The submission queue as the engine sees it: everything handed over,
   * in order, with queue_head marking how far it got. It is a queue and
   * not a batch because a linked chain can STOP in the middle of it -
   * see blocked below - and what follows has to wait where it is. */
  struct slipstream_sqeq queue;
  unsigned queue_head;
  int chain_failed; /* a linked op failed: cancel the rest of its chain */
  int blocked;      /* a linked blocking op is out at a worker; the queue waits */
  unsigned work_inflight;

  /* ---- the pool, and the one rule that keeps the ring SPSC ----------
   * Workers never post a completion. They run one syscall and hand the
   * result back through done_head; the engine is still the only thread
   * that writes a CQE and the only one that arms ring_fd, so the
   * completion ring stays single-producer/single-consumer and the pipe
   * invariant keeps exactly one writer. */
  mtx_t wq_mtx;
  cnd_t wq_cv;
  struct slipstream_work *wq_head, *wq_tail;     /* engine -> workers */
  struct slipstream_work *done_head, *done_tail; /* workers -> engine */
  unsigned wq_idle;
  int wq_stopping;
  thrd_t workers[SLIPSTREAM_WORKERS_MAX];
  unsigned workers_n;
};

struct io_uring_probe {
  int unused;
};

/* ---- prep: pure struct writers, exactly like liburing's -------------
 *
 * Not one of these makes a syscall in liburing either; they record what
 * the operation is. The engine below is where anything happens. */

static inline void io_uring_prep_nop(struct io_uring_sqe *s) {
  memset(s, 0, sizeof(*s));
  s->opcode = IORING_OP_NOP;
}
/* liburing's generic writer. A caller reaches for it where liburing
 * has no named helper yet - a URING_CMD spelled by hand, for instance -
 * and then sets the command's own fields itself. */
static inline void io_uring_prep_rw(int op, struct io_uring_sqe *s, int fd, const void *addr,
                                    unsigned len, uint64_t offset) {
  io_uring_prep_nop(s);
  s->opcode = (uint8_t)op;
  s->fd = fd;
  s->addr = (uint64_t)(uintptr_t)addr;
  s->len = len;
  s->off = offset;
}
static inline void io_uring_sqe_set_data64(struct io_uring_sqe *s, uint64_t d) { s->user_data = d; }
static inline uint64_t io_uring_cqe_get_data64(const struct io_uring_cqe *c) { return c->user_data; }

static inline void io_uring_prep_unlink(struct io_uring_sqe *s, const char *path, int flags) {
  io_uring_prep_nop(s);
  s->opcode = IORING_OP_UNLINKAT;
  s->fd = AT_FDCWD;
  s->addr = (uint64_t)(uintptr_t)path;
  s->unlink_flags = (uint32_t)flags;
}
static inline void io_uring_prep_socket_direct(struct io_uring_sqe *s, int domain, int type,
                                               int protocol, unsigned file_index, unsigned flags) {
  io_uring_prep_nop(s);
  s->opcode = IORING_OP_SOCKET;
  s->fd = domain;
  s->off = (uint64_t)type;
  s->len = (uint32_t)protocol;
  s->msg_flags = flags;
  s->file_index = file_index + 1; /* liburing's +1 encoding, kept */
}
static inline void io_uring_prep_cmd_sock(struct io_uring_sqe *s, int cmd_op, int fd, int level,
                                          int optname, void *optval, int optlen) {
  io_uring_prep_nop(s);
  s->opcode = IORING_OP_URING_CMD;
  s->cmd_op = (uint32_t)cmd_op;
  s->fd = fd;
  s->level = (uint32_t)level;
  s->optname = (uint32_t)optname;
  s->optval = (uint64_t)(uintptr_t)optval;
  s->optlen = (uint32_t)optlen;
}
static inline void io_uring_prep_bind(struct io_uring_sqe *s, int fd, struct sockaddr *addr,
                                      socklen_t addrlen) {
  io_uring_prep_nop(s);
  s->opcode = IORING_OP_BIND;
  s->fd = fd;
  s->addr = (uint64_t)(uintptr_t)addr;
  s->off = addrlen;
}
static inline void io_uring_prep_listen(struct io_uring_sqe *s, int fd, int backlog) {
  io_uring_prep_nop(s);
  s->opcode = IORING_OP_LISTEN;
  s->fd = fd;
  s->len = (uint32_t)backlog;
}
static inline void io_uring_prep_multishot_accept_direct(struct io_uring_sqe *s, int fd,
                                                         struct sockaddr *addr, socklen_t *addrlen,
                                                         int flags) {
  io_uring_prep_nop(s);
  s->opcode = IORING_OP_ACCEPT;
  s->fd = fd;
  s->addr = (uint64_t)(uintptr_t)addr;
  s->off = (uint64_t)(uintptr_t)addrlen;
  s->msg_flags = (uint32_t)flags;
}
static inline void io_uring_prep_recv(struct io_uring_sqe *s, int fd, void *buf, size_t len,
                                      int flags) {
  io_uring_prep_nop(s);
  s->opcode = IORING_OP_RECV;
  s->fd = fd;
  s->addr = (uint64_t)(uintptr_t)buf;
  s->len = (uint32_t)len;
  s->msg_flags = (uint32_t)flags;
}
static inline void io_uring_prep_recv_multishot(struct io_uring_sqe *s, int fd, void *buf,
                                                size_t len, int flags) {
  io_uring_prep_recv(s, fd, buf, len, flags);
}
static inline void io_uring_prep_send(struct io_uring_sqe *s, int fd, const void *buf, size_t len,
                                      int flags) {
  io_uring_prep_nop(s);
  s->opcode = IORING_OP_SEND;
  s->fd = fd;
  s->addr = (uint64_t)(uintptr_t)buf;
  s->len = (uint32_t)len;
  s->msg_flags = (uint32_t)flags;
}
static inline void io_uring_prep_sendmsg(struct io_uring_sqe *s, int fd, const struct msghdr *msg,
                                         unsigned flags) {
  io_uring_prep_nop(s);
  s->opcode = IORING_OP_SENDMSG;
  s->fd = fd;
  s->addr = (uint64_t)(uintptr_t)msg;
  s->len = 1;
  s->msg_flags = flags;
}
static inline void io_uring_prep_shutdown(struct io_uring_sqe *s, int fd, int how) {
  io_uring_prep_nop(s);
  s->opcode = IORING_OP_SHUTDOWN;
  s->fd = fd;
  s->len = (uint32_t)how;
}
static inline void io_uring_prep_close_direct(struct io_uring_sqe *s, unsigned file_index) {
  io_uring_prep_nop(s);
  s->opcode = IORING_OP_CLOSE;
  s->file_index = file_index + 1;
}
/* The raw-fd close, liburing's other spelling of it: file_index stays 0,
 * which is how the engine tells the two apart. A file opened with
 * io_uring_prep_openat comes back as a plain descriptor, and this is
 * how it goes away again - through the ring, on the engine, so it
 * cannot be closed while the engine is holding it. */
static inline void io_uring_prep_close(struct io_uring_sqe *s, int fd) {
  io_uring_prep_nop(s);
  s->opcode = IORING_OP_CLOSE;
  s->fd = fd;
}
static inline void io_uring_prep_poll_add(struct io_uring_sqe *s, int fd, unsigned poll_mask) {
  io_uring_prep_nop(s);
  s->opcode = IORING_OP_POLL_ADD;
  s->fd = fd;
  s->poll32_events = poll_mask;
}

/* One poll, armed once, reporting readiness for as long as it is left
 * alone. Every completion but the last carries IORING_CQE_F_MORE. */
static inline void io_uring_prep_multishot_poll_add(struct io_uring_sqe *s, int fd,
                                                    unsigned poll_mask) {
  io_uring_prep_poll_add(s, fd, poll_mask);
  s->len = IORING_POLL_ADD_MULTI;
}

/* Everything armed on this descriptor comes off, each armed op
 * completing with -ECANCELED so nobody waits for a completion that is
 * never coming. The cancel itself answers how many it took, or -ENOENT
 * when there was nothing.
 *
 * This is the one cancel this header needs. A poll remove, a timeout
 * remove and a cancel-by-user_data would be three ways to say the same
 * sentence, and each would oblige a caller to remember which kind of op
 * it armed. A descriptor is a thing the caller has in hand already. */
static inline void io_uring_prep_cancel_fd(struct io_uring_sqe *s, int fd, unsigned flags) {
  io_uring_prep_nop(s);
  s->opcode = IORING_OP_ASYNC_CANCEL;
  s->fd = fd;
  s->len = flags | IORING_ASYNC_CANCEL_FD;
}

/* Take an armed poll off the ring, named by the user_data it was armed
 * with. The removal itself completes, so a caller always learns whether
 * there was anything there to remove (-ENOENT if not). */
static inline void io_uring_prep_poll_remove(struct io_uring_sqe *s, uint64_t user_data) {
  io_uring_prep_nop(s);
  s->opcode = IORING_OP_POLL_REMOVE;
  s->fd = -1;
  s->addr = user_data;
}

/* Change an armed poll in place: what it waits for, what it answers
 * under, or both - and NOTHING is re-registered, which is the whole
 * point. Removing and adding would drop readiness that arrived in
 * between; this cannot. */
static inline void io_uring_prep_poll_update(struct io_uring_sqe *s, uint64_t old_user_data,
                                             uint64_t new_user_data, unsigned poll_mask,
                                             unsigned flags) {
  io_uring_prep_nop(s);
  s->opcode = IORING_OP_POLL_REMOVE;
  s->fd = -1;
  s->addr = old_user_data;
  s->off = new_user_data;
  s->len = flags;
  s->poll32_events = poll_mask;
}

/* ---- file IO: the ops the engine thread made possible ---------------
 *
 * These are BLOCKING syscalls run on the engine. select(2) cannot help
 * with a regular file - it reports every one of them ready, instantly,
 * whether or not the data is in page cache - so the only honest way to
 * offer them is to have somebody else block on them. That somebody is
 * a work thread, never the caller and never the engine. */
static inline void io_uring_prep_openat(struct io_uring_sqe *s, int dfd, const char *path,
                                        int flags, mode_t mode) {
  io_uring_prep_nop(s);
  s->opcode = IORING_OP_OPENAT;
  s->fd = dfd;
  s->addr = (uint64_t)(uintptr_t)path;
  s->open_flags = (uint32_t)flags;
  s->len = (uint32_t)mode;
}
static inline void io_uring_prep_read(struct io_uring_sqe *s, int fd, void *buf, unsigned nbytes,
                                      uint64_t offset) {
  io_uring_prep_nop(s);
  s->opcode = IORING_OP_READ;
  s->fd = fd;
  s->addr = (uint64_t)(uintptr_t)buf;
  s->len = nbytes;
  s->off = offset; /* (uint64_t)-1 means "wherever the fd is", like the kernel's */
}
static inline void io_uring_prep_statx(struct io_uring_sqe *s, int dfd, const char *path, int flags,
                                       unsigned mask, struct statx *statxbuf) {
  io_uring_prep_nop(s);
  s->opcode = IORING_OP_STATX;
  s->fd = dfd;
  s->addr = (uint64_t)(uintptr_t)path;
  s->statx_flags = (uint32_t)flags;
  s->len = mask;
  s->off = (uint64_t)(uintptr_t)statxbuf;
}

/* ---- the engine -----------------------------------------------------
 *
 * Everything from here to the public queue functions runs on the engine
 * thread, except where a comment says CALLER. None of it is part of the
 * API: the slipstream_ prefix is the statement that the engine has no
 * surface a consumer could reach for. */

static inline int slipstream_sqeq_reserve(struct slipstream_sqeq *q, unsigned need) {
  unsigned cap = q->cap ? q->cap : 64u;
  struct io_uring_sqe *v;
  if (q->n + need <= q->cap) return 1;
  while (cap < q->n + need) cap *= 2u;
  v = (struct io_uring_sqe *)realloc(q->v, (size_t)cap * sizeof(*v));
  if (!v) return 0;
  q->v = v;
  q->cap = cap;
  return 1;
}
/* The whole batch in one copy: the handoff is the only place an SQE is
 * ever duplicated, so it is one memcpy and not a loop of assignments. */
static inline int slipstream_sqeq_append(struct slipstream_sqeq *q, const struct io_uring_sqe *v,
                                         unsigned n) {
  if (!slipstream_sqeq_reserve(q, n)) return 0;
  memcpy(q->v + q->n, v, (size_t)n * sizeof(*v));
  q->n += n;
  return 1;
}
static inline int slipstream_sqeq_push(struct slipstream_sqeq *q, const struct io_uring_sqe *s) {
  if (!slipstream_sqeq_reserve(q, 1)) return 0;
  q->v[q->n++] = *s;
  return 1;
}
static inline void slipstream_sqeq_remove(struct slipstream_sqeq *q, unsigned i) {
  q->v[i] = q->v[q->n - 1];
  q->n--;
}
static inline void slipstream_sqeq_free(struct slipstream_sqeq *q) {
  free(q->v);
  q->v = NULL;
  q->n = q->cap = 0;
}
static inline int slipstream_cqq_push(struct slipstream_cqq *q, const struct io_uring_cqe *c) {
  if (q->n == q->cap) {
    unsigned cap = q->cap ? q->cap * 2u : 64u;
    struct io_uring_cqe *v = (struct io_uring_cqe *)malloc((size_t)cap * sizeof(*v));
    unsigned i;
    if (!v) return 0;
    for (i = 0; i < q->n; i++) v[i] = q->v[(q->head + i) % q->cap];
    free(q->v);
    q->v = v;
    q->cap = cap;
    q->head = 0;
  }
  q->v[(q->head + q->n) % q->cap] = *c;
  q->n++;
  return 1;
}
static inline void slipstream_cqq_free(struct slipstream_cqq *q) {
  free(q->v);
  q->v = NULL;
  q->head = q->n = q->cap = 0;
}

/* CALLER. Wake the engine out of select. A full control pipe means a
 * wakeup is already pending, which is what this call wanted, so the
 * failed write is the success case. */
static inline void slipstream_poke(struct io_uring *st) {
  const unsigned char b = 1;
  ssize_t r = write(st->ctl_w, &b, 1);
  (void)r;
}

/* ENGINE. Publish one completion. Once the ring is full, or once
 * anything is already behind it, entries go to the backlog so FIFO
 * order survives; the caller's next advance makes room and pokes. */
static inline void slipstream_post(struct io_uring *st, uint64_t ud, int32_t res, uint32_t flags) {
  struct io_uring_cqe c;
  unsigned head, tail;
  c.user_data = ud;
  c.res = res;
  c.flags = flags;
  head = slipstream_load(&st->cq_head, slipstream_mo_acquire);
  tail = slipstream_load(&st->cq_tail, slipstream_mo_relaxed);
  if (st->backlog.n != 0 || (tail - head) >= SLIPSTREAM_CQ_DEPTH) {
    /* The one place a completion can be lost is here, and only if the
     * host is out of memory while the caller is not draining. */
    if (slipstream_cqq_push(&st->backlog, &c)) slipstream_store(&st->cq_overflow, 1, slipstream_mo_relaxed);
    return;
  }
  st->cqes[tail & SLIPSTREAM_CQ_MASK] = c;
  /* Release: the slot's contents are what this hands over. Every walk on
   * the caller's side loads the tail with acquire. */
  slipstream_store(&st->cq_tail, tail + 1u, slipstream_mo_release);
}

/* ENGINE. Push as much of the backlog into the ring as fits. */
static inline void slipstream_flush_backlog(struct io_uring *st) {
  while (st->backlog.n != 0) {
    const unsigned head = slipstream_load(&st->cq_head, slipstream_mo_acquire);
    const unsigned tail = slipstream_load(&st->cq_tail, slipstream_mo_relaxed);
    if ((tail - head) >= SLIPSTREAM_CQ_DEPTH) break;
    st->cqes[tail & SLIPSTREAM_CQ_MASK] = st->backlog.v[st->backlog.head];
    st->backlog.head = (st->backlog.head + 1u) % st->backlog.cap;
    st->backlog.n--;
    slipstream_store(&st->cq_tail, tail + 1u, slipstream_mo_release);
  }
  slipstream_store(&st->cq_overflow, st->backlog.n != 0, slipstream_mo_relaxed);
}

/* ENGINE. Half of the ring_fd invariant documented on the struct: make
 * the pipe readable when completions are pending. The head re-check
 * under the lock is what keeps a byte from outliving the completions it
 * announced. */
static inline void slipstream_arm_cq_pipe(struct io_uring *st) {
  const unsigned char b = 1;
  /* The ONLY check that may be made outside the lock is the cursor one,
   * and only in this direction: an empty ring has nothing to announce,
   * and the engine is the only thread that can make it non-empty. The
   * armed flag must NOT be pre-checked here - reading it as "already
   * armed" a moment before the caller drains the byte loses the wakeup
   * for the completions just published, and a lost wakeup is a hung
   * embedder. (It was written that way once. The overflow test hung.) */
  if (slipstream_load(&st->cq_head, slipstream_mo_acquire) ==
      slipstream_load(&st->cq_tail, slipstream_mo_relaxed))
    return;
  mtx_lock(&st->pipe_mtx);
  if (!slipstream_load(&st->cq_armed, slipstream_mo_relaxed) &&
      slipstream_load(&st->cq_head, slipstream_mo_acquire) !=
          slipstream_load(&st->cq_tail, slipstream_mo_relaxed)) {
    if (write(st->cq_w, &b, 1) == 1) slipstream_store(&st->cq_armed, 1, slipstream_mo_relaxed);
    /* A write can only fail because the pipe is full, and a full pipe is
     * a readable pipe: the property this call exists for already holds,
     * and the mark stays clear so the next publish tries again. */
  }
  mtx_unlock(&st->pipe_mtx);
}

/* CALLER. The other half: drink the pipe empty once the ring is empty.
 * The tail re-check under the lock mirrors the engine's head re-check -
 * completions that arrived while this was being decided keep their
 * byte. */
static inline void slipstream_drain_cq_pipe(struct io_uring *st) {
  unsigned char b[64];
  /* The mirror image, and the same rule: an advance that left
   * completions behind must keep the byte, and that is decidable
   * without the lock because only this thread moves the head. The armed
   * flag is read inside the lock, for the reason given above. So the
   * lock is taken once per DRAINED batch, not once per completion. */
  if (slipstream_load(&st->cq_head, slipstream_mo_relaxed) !=
      slipstream_load(&st->cq_tail, slipstream_mo_acquire))
    return;
  mtx_lock(&st->pipe_mtx);
  if (slipstream_load(&st->cq_armed, slipstream_mo_relaxed) &&
      slipstream_load(&st->cq_head, slipstream_mo_relaxed) ==
          slipstream_load(&st->cq_tail, slipstream_mo_acquire)) {
    while (read(st->ring_fd, b, sizeof(b)) > 0) {
    }
    slipstream_store(&st->cq_armed, 0, slipstream_mo_relaxed);
  }
  mtx_unlock(&st->pipe_mtx);
}

/* CALLER. How many completions there are to walk. */
static inline unsigned slipstream_cq_ready(struct io_uring *st) {
  return slipstream_load(&st->cq_tail, slipstream_mo_acquire) -
         slipstream_load(&st->cq_head, slipstream_mo_relaxed);
}

static inline int slipstream_deferred(uint8_t op) {
  return op == IORING_OP_ACCEPT || op == IORING_OP_RECV || op == IORING_OP_SEND ||
         op == IORING_OP_SENDMSG || op == IORING_OP_POLL_ADD;
}

/* WORK THREAD. The blocking half of the operation set, and the only
 * code in this file that runs anywhere but the engine or the caller. It
 * takes no struct io_uring: everything it needs is in the copy it was
 * handed - which is why it cannot race with anything the engine owns. */
static inline int slipstream_execute_blocking(const struct io_uring_sqe *s) {
  switch (s->opcode) {
    case IORING_OP_OPENAT: {
      const char *path = (const char *)(uintptr_t)s->addr;
      const int fd = openat(s->fd, path, (int)s->open_flags | O_CLOEXEC, (mode_t)s->len);
      if (fd < 0) return -errno;
      /* The published ceiling has to hold for every descriptor this API
       * hands out, not only for sockets - a consumer that got one above
       * it would have been told a number that is not true. */
      if (fd >= IO_URING_FD_CEILING) {
        close(fd);
        return -EMFILE;
      }
      return fd;
    }
    case IORING_OP_READ: {
      void *buf = (void *)(uintptr_t)s->addr;
      /* The offset is the kernel's convention: (uint64_t)-1 means "use
       * the file position". */
      const ssize_t r = (s->off == (uint64_t)-1) ? read(s->fd, buf, s->len)
                                                 : pread(s->fd, buf, s->len, (off_t)s->off);
      return r >= 0 ? (int)r : -errno;
    }
    case IORING_OP_STATX: {
#if defined(STATX_BASIC_STATS)
      const char *path = (const char *)(uintptr_t)s->addr;
      struct statx *stx = (struct statx *)(uintptr_t)s->off;
      return statx(s->fd, path, (int)s->statx_flags, s->len, stx) == 0 ? 0 : -errno;
#else
      /* No statx on this host's headers. Saying so is the whole answer:
       * a caller that needs file metadata learns it here, instead of
       * being handed a struct filled in from something else. */
      return -EOPNOTSUPP;
#endif
    }
    case IORING_OP_CLOSE:
      return close(s->fd) == 0 ? 0 : -errno;
    default:
      return -EOPNOTSUPP;
  }
}

/* Blocking work, as opposed to readiness work. A CLOSE counts only when
 * it names a raw descriptor: closing a DIRECT one edits the engine's
 * table and drops what was armed on that slot, so it stays where that
 * state lives. */
static inline int slipstream_blocking(const struct io_uring_sqe *s) {
  return s->opcode == IORING_OP_OPENAT || s->opcode == IORING_OP_READ ||
         s->opcode == IORING_OP_STATX || (s->opcode == IORING_OP_CLOSE && s->file_index == 0);
}

static inline int slipstream_resolve_fd(struct io_uring *st, const struct io_uring_sqe *s) {
  if (s->flags & IOSQE_FIXED_FILE) {
    const uint32_t slot = (uint32_t)s->fd;
    if (slot >= st->files_n) return -1;
    return st->files[slot];
  }
  return s->fd;
}

/* ENGINE. The ops that answer without waiting for readiness - each
 * decoded from the fields the prep_* writer above recorded. These used
 * to run inside io_uring_submit on the caller's thread; they run here
 * now, in submission order, which is what keeps a chain's members in
 * their stated order relative to everything submitted around them. The
 * file ops are in this set too, and they are the ones that can actually
 * take a while: that is the trade the engine exists to make. */
static inline int slipstream_execute(struct io_uring *st, struct io_uring_sqe *s) {
  switch (s->opcode) {
    case IORING_OP_NOP:
      return 0;
    case IORING_OP_ASYNC_CANCEL: {
      /* Runs on the engine, because the armed queue is the engine's.
       * Walks backwards so removing an entry cannot skip the next one. */
      unsigned i = st->waiting.n;
      int taken = 0;
      while (i-- > 0) {
        struct io_uring_sqe *w = &st->waiting.v[i];
        if (slipstream_resolve_fd(st, w) != s->fd) continue;
        slipstream_post(st, w->user_data, -ECANCELED, 0);
        /* A quiet multishot poll still counts against poll_quiet, and
         * taking it away without saying so would leave the engine
         * waking itself forever. */
        if (w->opcode == IORING_OP_POLL_ADD && w->off != 0) {
          slipstream_store(&st->poll_quiet,
                           slipstream_load(&st->poll_quiet, slipstream_mo_relaxed) - 1u,
                           slipstream_mo_release);
        }
        slipstream_sqeq_remove(&st->waiting, i);
        taken++;
        if (!(s->len & IORING_ASYNC_CANCEL_ALL)) break;
      }
      return taken != 0 ? taken : -ENOENT;
    }
    case IORING_OP_POLL_REMOVE: {
      /* Removal and update are ONE opcode, the way liburing spells them:
       * an update is a removal that puts something back in the same
       * place. It runs here, on the engine, because the armed queue is
       * the engine's and nobody else may touch it.
       *
       * In place matters. Remove-then-add would drop any readiness that
       * arrived between the two, and would hand the caller a new
       * registration where it asked for a changed one. */
      unsigned i;
      for (i = 0; i < st->waiting.n; i++) {
        struct io_uring_sqe *w = &st->waiting.v[i];
        if (w->opcode != IORING_OP_POLL_ADD || w->user_data != s->addr) continue;
        if (s->len & (IORING_POLL_UPDATE_EVENTS | IORING_POLL_UPDATE_USER_DATA)) {
          if (s->len & IORING_POLL_UPDATE_EVENTS) w->poll32_events = s->poll32_events;
          if (s->len & IORING_POLL_UPDATE_USER_DATA) w->user_data = s->off;
          return 0;
        }
        /* A cancelled poll completes, the same as it does on a real
         * ring - so whoever armed it hears that it is over instead of
         * waiting for a completion that is never coming. */
        slipstream_post(st, w->user_data, -ECANCELED, 0);
        slipstream_sqeq_remove(&st->waiting, i);
        return 0;
      }
      return -ENOENT;
    }
    case IORING_OP_UNLINKAT: {
      const char *path = (const char *)(uintptr_t)s->addr;
      return unlinkat(s->fd, path, (int)s->unlink_flags) == 0 ? 0 : -errno;
    }
    case IORING_OP_SOCKET: {
      const int fd = socket(s->fd, (int)s->off | SOCK_NONBLOCK | SOCK_CLOEXEC, (int)s->len);
      uint32_t slot;
      if (fd < 0) return -errno;
      if (fd >= IO_URING_FD_CEILING) { /* the assertion that can structurally never fire */
        close(fd);
        return -EMFILE;
      }
      slot = s->file_index - 1;
      if (slot >= st->files_n) {
        close(fd);
        return -EINVAL;
      }
      st->files[slot] = fd;
      return 0;
    }
    case IORING_OP_URING_CMD: {
      const int fd = slipstream_resolve_fd(st, s);
      if (fd < 0) return -EBADF;
      switch (s->cmd_op) {
        case SOCKET_URING_OP_SETSOCKOPT: {
          const void *val = (const void *)(uintptr_t)s->optval;
          return setsockopt(fd, (int)s->level, (int)s->optname, val, s->optlen) == 0 ? 0 : -errno;
        }
        case SOCKET_URING_OP_GETSOCKOPT: {
          /* The kernel answers the LENGTH in the cqe's res and writes
           * the value into optval; optlen is what the caller offered. */
          socklen_t len = (socklen_t)s->optlen;
          void *val = (void *)(uintptr_t)s->optval;
          if (getsockopt(fd, (int)s->level, (int)s->optname, val, &len) != 0) return -errno;
          return (int)len;
        }
        case SOCKET_URING_OP_GETSOCKNAME: {
          /* io_uring's own shape for this one (io_uring/cmd_net.c):
           * addr is the sockaddr buffer, optval points at the length
           * (in AND out), and optlen picks the side - 0 is this
           * socket's own name, 1 is the peer's. */
          struct sockaddr *sa = (struct sockaddr *)(uintptr_t)s->addr;
          int *slen = (int *)(uintptr_t)s->optval;
          socklen_t len;
          int rc;
          if (sa == NULL || slen == NULL) return -EFAULT;
          len = (socklen_t)*slen;
          rc = s->optlen ? getpeername(fd, sa, &len) : getsockname(fd, sa, &len);
          if (rc != 0) return -errno;
          *slen = (int)len;
          return 0;
        }
        default: return -EOPNOTSUPP;
      }
    }
    case IORING_OP_BIND: {
      const int fd = slipstream_resolve_fd(st, s);
      const struct sockaddr *sa = (const struct sockaddr *)(uintptr_t)s->addr;
      if (fd < 0) return -EBADF;
      return bind(fd, sa, (socklen_t)s->off) == 0 ? 0 : -errno;
    }
    case IORING_OP_LISTEN: {
      const int fd = slipstream_resolve_fd(st, s);
      if (fd < 0) return -EBADF;
      return listen(fd, (int)s->len) == 0 ? 0 : -errno;
    }
    case IORING_OP_SHUTDOWN: {
      const int fd = slipstream_resolve_fd(st, s);
      if (fd < 0) return -EBADF;
      return shutdown(fd, (int)s->len) == 0 ? 0 : -errno;
    }
    case IORING_OP_CLOSE: {
      uint32_t slot;
      unsigned i;
      /* The slot is file_index-1. Closing also drops whatever was armed
       * on that fd - the kernel would post -ECANCELED, but the Ring's
       * gen guard ignores those anyway; not existing is as good as being
       * ignored. That this runs on the engine, between two select
       * passes, is the point: the descriptor cannot be closed while the
       * same engine is holding it in an fd_set. */
      slot = s->file_index - 1;
      if (slot >= st->files_n || st->files[slot] < 0) return -EBADF;
      close(st->files[slot]);
      st->files[slot] = -1;
      for (i = 0; i < st->waiting.n;) {
        if ((uint32_t)st->waiting.v[i].fd == slot && (st->waiting.v[i].flags & IOSQE_FIXED_FILE) != 0)
          slipstream_sqeq_remove(&st->waiting, i);
        else
          i++;
      }
      if (st->free_n < st->free_cap) st->free_slots[st->free_n++] = slot;
      return 0;
    }
    default:
      return -EOPNOTSUPP;
  }
}

/* ENGINE. Give voice back to every quiet poll whose completion the
 * caller has now taken. Called once per turn, before the sets are built,
 * so a poll that has been caught up with is armed again in the very same
 * turn rather than a turn late. */
static inline void slipstream_poll_voice(struct io_uring *st) {
  const unsigned head = slipstream_load(&st->cq_head, slipstream_mo_acquire);
  unsigned freed = 0;
  unsigned i;
  if (slipstream_load(&st->poll_quiet, slipstream_mo_relaxed) == 0) return;
  for (i = 0; i < st->waiting.n; i++) {
    struct io_uring_sqe *w = &st->waiting.v[i];
    if (w->opcode != IORING_OP_POLL_ADD || w->off == 0) continue;
    /* Unsigned difference, so the comparison survives the counters
     * wrapping - the same way every other head/tail test here does. */
    if ((unsigned)(head - (unsigned)w->off) > 0x80000000u) continue;
    w->off = 0;
    freed++;
  }
  if (freed != 0) {
    slipstream_store(&st->poll_quiet,
                     slipstream_load(&st->poll_quiet, slipstream_mo_relaxed) - freed,
                     slipstream_mo_release);
  }
}

/* ENGINE. Builds the fd_sets for everything armed, PLUS the control
 * pipe - that last one is what lets a submission, an errand or a
 * shutdown interrupt a select that would otherwise wait on the network.
 * Returns the nfds argument select(2) wants. */
static inline int slipstream_build_waitsets(struct io_uring *st, fd_set *rset, fd_set *wset) {
  int nfds = 0;
  unsigned i;
  FD_ZERO(rset);
  FD_ZERO(wset);
  for (i = 0; i < st->waiting.n; i++) {
    const struct io_uring_sqe *w = &st->waiting.v[i];
    const int fd = slipstream_resolve_fd(st, w);
    if (fd < 0 || fd >= IO_URING_FD_CEILING) continue;
    if (w->opcode == IORING_OP_SEND || w->opcode == IORING_OP_SENDMSG) FD_SET(fd, wset);
    else if (w->opcode == IORING_OP_POLL_ADD) {
      /* A poll goes where its MASK says, which is the whole difference
       * between this and every other op here: those know their own
       * direction, a poll is told. Both sets when it wants both.
       *
       * POLLIN and POLLOUT are the whole vocabulary, because they are
       * the whole of what select(2) has to say. A mask asking for
       * anything else gets the readable set - the closest honest
       * answer, and better than arming nothing at all. */
      /* A quiet poll is in NEITHER set. Leaving it in would put a ready
       * descriptor in front of select, which would return at once, over
       * and over - the spin this whole mechanism exists to prevent. */
      if (w->off != 0) continue;
      if (w->poll32_events & POLLOUT) FD_SET(fd, wset);
      if (!(w->poll32_events & POLLOUT) || (w->poll32_events & POLLIN)) FD_SET(fd, rset);
    } else FD_SET(fd, rset);
    if (fd + 1 > nfds) nfds = fd + 1;
  }
  FD_SET(st->ctl_r, rset);
  if (st->ctl_r + 1 > nfds) nfds = st->ctl_r + 1;
  return nfds;
}

/* ENGINE. Dispatches whatever select(2) found ready into completions. */
static inline void slipstream_drain_ready(struct io_uring *st, const fd_set *rset,
                                          const fd_set *wset) {
  unsigned i;
  for (i = 0; i < st->waiting.n;) {
    struct io_uring_sqe *w = &st->waiting.v[i];
    const int fd = slipstream_resolve_fd(st, w);
    int remove = 0;
    if (fd >= 0 && fd < IO_URING_FD_CEILING) {
      if (w->opcode == IORING_OP_SEND && FD_ISSET(fd, wset)) {
        const void *buf = (const void *)(uintptr_t)w->addr;
        const ssize_t r = send(fd, buf, w->len, (int)w->msg_flags | MSG_NOSIGNAL);
        slipstream_post(st, w->user_data, r >= 0 ? (int)r : -errno, 0);
        remove = 1;
      } else if (w->opcode == IORING_OP_SENDMSG && FD_ISSET(fd, wset)) {
        /* The delivery plan (#168): head plus pointers into the asset
         * mapping, handed over as one msghdr. prep_sendmsg puts the
         * msghdr POINTER in addr, and the Ring keeps that msghdr in the
         * connection, so it is still alive when this deferred op runs. */
        struct msghdr *m = (struct msghdr *)(uintptr_t)w->addr;
        const ssize_t r = sendmsg(fd, m, (int)w->msg_flags | MSG_NOSIGNAL);
        slipstream_post(st, w->user_data, r >= 0 ? (int)r : -errno, 0);
        remove = 1;
      } else if (w->opcode == IORING_OP_ACCEPT && FD_ISSET(fd, rset)) {
        const int nf = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (nf >= 0) {
          if (nf >= IO_URING_FD_CEILING || st->free_n == 0) {
            close(nf);
            slipstream_post(st, w->user_data, -ENFILE, 0); /* no MORE: the Ring re-arms */
            remove = 1;
          } else {
            const uint32_t slot = st->free_slots[--st->free_n];
            st->files[slot] = nf;
            slipstream_post(st, w->user_data, (int)slot, IORING_CQE_F_MORE);
          }
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
          slipstream_post(st, w->user_data, -errno, IORING_CQE_F_MORE); /* transient, stays armed */
        }
      } else if (w->opcode == IORING_OP_RECV && FD_ISSET(fd, rset)) {
        struct io_uring_buf_ring *br = &st->bufring;
        const uint32_t brhead = slipstream_load(&br->head, slipstream_mo_relaxed);
        if (slipstream_load(&br->tail, slipstream_mo_acquire) == brhead) {
          slipstream_post(st, w->user_data, -ENOBUFS, 0); /* no MORE: the Ring re-arms */
          remove = 1;
        } else {
          const struct io_uring_buf *e = &br->bufs[brhead & br->mask];
          const ssize_t r = recv(fd, e->addr, e->len, 0);
          if (r > 0) {
            slipstream_store(&br->head, brhead + 1u, slipstream_mo_relaxed); /* strictly in ring order */
            slipstream_post(st, w->user_data, (int)r,
                            IORING_CQE_F_MORE | IORING_CQE_F_BUFFER |
                                ((uint32_t)e->bid << IORING_CQE_BUFFER_SHIFT));
          } else if (r == 0) {
            slipstream_post(st, w->user_data, 0, 0); /* EOF ends the multishot, no buffer used */
            remove = 1;
          } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            slipstream_post(st, w->user_data, -errno, 0);
            remove = 1;
          }
        }
      } else if (w->opcode == IORING_OP_POLL_ADD &&
                 (FD_ISSET(fd, rset) || FD_ISSET(fd, wset))) {
        /* What select knows, and NOTHING beyond it. This engine is
         * select(2) - asking poll(2) here for a nicer answer would
         * quietly make it a poll engine, which is the one thing this
         * file is not.
         *
         * So there is no POLLHUP and no POLLERR in these revents, and
         * there cannot be: select reports a dead peer as readable and
         * says no more than that. A reader learns the difference the
         * way anyone using select learns it - recv answers 0 for a
         * closed peer and -1 for a broken one. That is a real
         * difference from a kernel ring, and it belongs in the docs
         * rather than in a syscall smuggled in here. */
        unsigned got = 0;
        if (FD_ISSET(fd, rset)) got |= POLLIN;
        if (FD_ISSET(fd, wset)) got |= POLLOUT;
        got &= w->poll32_events ? w->poll32_events : (unsigned)POLLIN;
        if (got == 0) {
          /* Ready for a direction this poll never asked about. Not this
           * one's completion, and no reason for it to end. */
        } else if (w->len & IORING_POLL_ADD_MULTI) {
          /* Armed once, still armed: MORE says so, and the entry stays
           * in the queue. Only POLL_REMOVE takes it out.
           *
           * And then it goes QUIET until the caller takes that
           * completion. select is level-triggered: an fd nobody has read
           * yet stays ready, so without this the engine would post for
           * every turn of the loop - measured at 1.6M completions and a
           * whole core in two seconds, for ONE unread byte. A kernel
           * ring does not do that, because its multishot poll hangs off
           * a waitqueue and only fires on a wakeup. One outstanding
           * completion per poll is how that is honoured here, and it
           * loses nothing: the fd is still ready, so the next turn after
           * the caller catches up reports it again. */
          slipstream_post(st, w->user_data, (int)got, IORING_CQE_F_MORE);
          w->off = (uint64_t)slipstream_load(&st->cq_tail, slipstream_mo_relaxed);
          slipstream_store(&st->poll_quiet,
                           slipstream_load(&st->poll_quiet, slipstream_mo_relaxed) + 1u,
                           slipstream_mo_release);
        } else {
          slipstream_post(st, w->user_data, (int)got, 0);
          remove = 1;
        }
      }
    } else {
      slipstream_post(st, w->user_data, -EBADF, 0);
      remove = 1;
    }
    if (remove) slipstream_sqeq_remove(&st->waiting, i);
    else i++;
  }
}

/* ENGINE. select said EBADF (or something else it will keep saying):
 * with a whole thread of its own, retrying that in a loop is a spinning
 * CPU, not a hiccup. So find the armed op whose descriptor died under it
 * - a raw fd the caller closed behind the API's back is the only way to
 * get here - complete it and move on. */
static inline void slipstream_sweep_dead(struct io_uring *st) {
  unsigned i;
  for (i = 0; i < st->waiting.n;) {
    const int fd = slipstream_resolve_fd(st, &st->waiting.v[i]);
    if (fd < 0 || fd >= IO_URING_FD_CEILING || fcntl(fd, F_GETFD) < 0) {
      slipstream_post(st, st->waiting.v[i].user_data, -EBADF, 0);
      slipstream_sqeq_remove(&st->waiting, i);
    } else {
      i++;
    }
  }
}

static inline void slipstream_drain_ctl(struct io_uring *st) {
  unsigned char b[64];
  while (read(st->ctl_r, b, sizeof(b)) > 0) {
  }
}

/* ENGINE. One errand, run between two select passes. */
static inline void slipstream_run_errand(struct io_uring *st, struct slipstream_errand *e) {
  switch (e->kind) {
    case SLIPSTREAM_ERRAND_FILES_SPARSE: {
      int *v = (int *)realloc(st->files, (size_t)e->a * sizeof(int));
      unsigned i;
      if (!v && e->a != 0) {
        e->rc = -ENOMEM;
        return;
      }
      st->files = v;
      st->files_n = e->a;
      for (i = 0; i < st->files_n; i++) st->files[i] = -1;
      e->rc = 0;
      return;
    }
    case SLIPSTREAM_ERRAND_ALLOC_RANGE: {
      uint32_t *v = (uint32_t *)realloc(st->free_slots, (size_t)e->b * sizeof(uint32_t));
      unsigned i;
      if (!v && e->b != 0) {
        e->rc = -ENOMEM;
        return;
      }
      st->free_slots = v;
      st->free_cap = e->b;
      st->free_n = 0;
      /* Descending, so allocation hands out low slots first (cosmetic
       * parity with the kernel; the Ring only checks the bound). */
      for (i = e->b; i-- > 0;) st->free_slots[st->free_n++] = e->a + i;
      e->rc = 0;
      return;
    }
    case SLIPSTREAM_ERRAND_FILES_UPDATE: {
      unsigned i;
      for (i = 0; i < e->b; i++) {
        if (e->a + i >= st->files_n) {
          e->rc = -EINVAL;
          return;
        }
        /* dup: the caller may close its copy after registering, exactly
         * like the kernel's table taking its own reference. */
        st->files[e->a + i] = dup(e->fds[i]);
      }
      e->rc = (int)e->b;
      return;
    }
    case SLIPSTREAM_ERRAND_SETUP_BUFRING: {
      struct io_uring_buf *v = (struct io_uring_buf *)calloc(e->a, sizeof(struct io_uring_buf));
      if (!v) {
        e->rc = -ENOMEM;
        return;
      }
      free(st->bufring.bufs);
      st->bufring.bufs = v;
      st->bufring.mask = e->a - 1u;
      slipstream_store(&st->bufring.head, 0u, slipstream_mo_relaxed);
      slipstream_store(&st->bufring.tail, 0u, slipstream_mo_relaxed);
      e->rc = 0;
      return;
    }
    default:
      e->rc = -EINVAL;
      return;
  }
}

/* WORK THREAD. Take one item, run it, hand the result back, poke the
 * engine. Nothing else: no completion ring, no fd_set, no direct table.
 *
 * On teardown the rule is "finish what is running, drop what is not":
 * an item already in a syscall is seen through - there is no portable
 * way to cancel a read - and its result is discarded, while items still
 * queued are freed unrun, because the ring they would complete into is
 * going away. */
static inline int slipstream_worker_run(void *arg) {
  struct io_uring *st = (struct io_uring *)arg;
  for (;;) {
    struct slipstream_work *w;
    mtx_lock(&st->wq_mtx);
    while (st->wq_head == NULL && !st->wq_stopping) {
      st->wq_idle++;
      cnd_wait(&st->wq_cv, &st->wq_mtx);
      st->wq_idle--;
    }
    if (st->wq_stopping) {
      mtx_unlock(&st->wq_mtx);
      break;
    }
    w = st->wq_head;
    st->wq_head = w->next;
    if (st->wq_head == NULL) st->wq_tail = NULL;
    mtx_unlock(&st->wq_mtx);

    w->res = slipstream_execute_blocking(&w->sqe);

    mtx_lock(&st->wq_mtx);
    w->next = NULL;
    if (st->done_tail != NULL) st->done_tail->next = w;
    else st->done_head = w;
    st->done_tail = w;
    mtx_unlock(&st->wq_mtx);
    slipstream_poke(st); /* the engine posts it; a worker never does */
  }
  return 0;
}

/* ENGINE. Hand one blocking op to the pool, growing it by one thread if
 * every worker is busy and the ceiling allows. The SQE is copied with
 * its descriptor already resolved, so the worker never reads the direct
 * table. */
static inline void slipstream_dispatch(struct io_uring *st, const struct io_uring_sqe *s,
                                       int linked) {
  struct slipstream_work *w;
  int need_worker;
  mtx_lock(&st->wq_mtx);
  need_worker = st->wq_idle == 0 && st->workers_n < SLIPSTREAM_WORKERS_MAX;
  mtx_unlock(&st->wq_mtx);
  if (need_worker) {
    if (thrd_create(&st->workers[st->workers_n], slipstream_worker_run, st) == thrd_success)
      st->workers_n++;
    else if (st->workers_n == 0) { /* nobody to run it, and no way to make one */
      slipstream_post(st, s->user_data, -EAGAIN, 0);
      return;
    }
  }
  w = (struct slipstream_work *)malloc(sizeof(*w));
  if (w == NULL) {
    slipstream_post(st, s->user_data, -ENOMEM, 0);
    return;
  }
  w->sqe = *s;
  w->res = 0;
  w->linked = linked;
  w->next = NULL;
  mtx_lock(&st->wq_mtx);
  if (st->wq_tail != NULL) st->wq_tail->next = w;
  else st->wq_head = w;
  st->wq_tail = w;
  cnd_signal(&st->wq_cv);
  mtx_unlock(&st->wq_mtx);
  st->work_inflight++;
}

/* ENGINE. Collect finished work and turn it into completions - here, on
 * this thread, because the ring has exactly one producer. A linked op
 * coming back is also what releases the rest of its chain. */
static inline void slipstream_reap_work(struct io_uring *st) {
  struct slipstream_work *list;
  mtx_lock(&st->wq_mtx);
  list = st->done_head;
  st->done_head = st->done_tail = NULL;
  mtx_unlock(&st->wq_mtx);
  while (list != NULL) {
    struct slipstream_work *w = list;
    list = w->next;
    slipstream_post(st, w->sqe.user_data, w->res, 0);
    if (w->linked) {
      st->blocked = 0;
      if (w->res < 0) st->chain_failed = 1;
    }
    st->work_inflight--;
    free(w);
  }
}

/* ENGINE. Everything submitted, in submission order, with link
 * semantics: a failing IOSQE_IO_LINK member cancels the rest of its
 * chain (-ECANCELED), which the Ring's setup chains and its
 * shutdown+close pair rely on.
 *
 * Three kinds of op and three answers. An immediate one runs here.  A
 * readiness one parks in `waiting`. A blocking one goes to the pool -
 * and if it is LINKED, the queue stops right here until the result
 * comes back, which is what keeps a chain that contains a file read in
 * its stated order: the engine holds the chain, the worker only
 * computes. A chain member behind a PARKED op is not held that way -
 * readiness is not an outcome that can fail, and holding it would need
 * the multishot ops to have an end, which they do not. */
static inline void slipstream_process(struct io_uring *st) {
  while (st->queue_head < st->queue.n) {
    struct io_uring_sqe *s = &st->queue.v[st->queue_head];
    const int linked = (s->flags & IOSQE_IO_LINK) != 0;
    int res;
    if (st->blocked) return; /* a linked blocking op is still out */
    if (st->chain_failed) {
      slipstream_post(st, s->user_data, -ECANCELED, 0);
      st->queue_head++;
      if (!linked) st->chain_failed = 0;
      continue;
    }
    if (slipstream_blocking(s)) {
      struct io_uring_sqe c = *s;
      if (c.flags & IOSQE_FIXED_FILE) {
        const int fd = slipstream_resolve_fd(st, &c);
        if (fd < 0) {
          slipstream_post(st, s->user_data, -EBADF, 0);
          st->queue_head++;
          if (linked) st->chain_failed = 1;
          continue;
        }
        c.fd = fd;
        c.flags &= (uint8_t)~IOSQE_FIXED_FILE;
      }
      st->queue_head++;
      if (linked) st->blocked = 1;
      slipstream_dispatch(st, &c, linked);
      continue;
    }
    if (slipstream_deferred(s->opcode)) {
      if (!slipstream_sqeq_push(&st->waiting, s)) slipstream_post(st, s->user_data, -ENOMEM, 0);
      st->queue_head++;
      continue;
    }
    res = slipstream_execute(st, s);
    slipstream_post(st, s->user_data, res, 0);
    st->queue_head++;
    if (linked && res < 0) st->chain_failed = 1;
  }
  st->queue.n = 0;
  st->queue_head = 0;
}

/* ENGINE. The whole of it: take work, run it, publish, sleep in select
 * until something happens. This is the kernel half's loop, and the only
 * loop in this file that runs on its own thread - the DRIVER's loop is
 * still the embedder's, and this one never calls into it. */
static inline int slipstream_engine_run(void *arg) {
  struct io_uring *st = (struct io_uring *)arg;
  struct slipstream_sqeq batch;
  memset(&batch, 0, sizeof(batch));
  for (;;) {
    struct slipstream_errand *errand = NULL;
    struct slipstream_sqeq tmp;
    fd_set rset, wset;
    int nfds, rc, stop;

    mtx_lock(&st->mtx);
    stop = st->stopping;
    if (!stop) {
      tmp = batch;
      batch = st->pending;
      st->pending = tmp; /* swap: the batch buffer goes back to be refilled */
      errand = st->errand;
      st->errand = NULL;
    }
    mtx_unlock(&st->mtx);
    if (stop) break;
    if (batch.n != 0) { /* appended, not executed: the queue may be held */
      if (!slipstream_sqeq_append(&st->queue, batch.v, batch.n)) {
        unsigned k;
        for (k = 0; k < batch.n; k++) slipstream_post(st, batch.v[k].user_data, -ENOMEM, 0);
      }
      batch.n = 0;
    }

    if (errand) {
      slipstream_run_errand(st, errand);
      mtx_lock(&st->mtx);
      errand->done = 1;
      cnd_signal(&errand->cv); /* errand is the caller's stack frame:
                                * nothing may touch it after the unlock */
      mtx_unlock(&st->mtx);
    }

    slipstream_flush_backlog(st);
    slipstream_process(st);
    slipstream_arm_cq_pipe(st);

    slipstream_poll_voice(st);
    nfds = slipstream_build_waitsets(st, &rset, &wset);
    mtx_lock(&st->mtx);
    if (st->pending.n != 0 || st->errand != NULL || st->stopping) {
      mtx_unlock(&st->mtx);
      continue;
    }
    /* THE BACKLOG MUST NOT BE SLEPT ON. The caller's poke from
     * io_uring_cq_advance is the fast path, and it is not sufficient on
     * its own: the caller reads cq_overflow right after freeing the
     * space, and this thread can set that flag a moment LATER - one
     * post() that read a stale head decides the ring is full, and from
     * then on FIFO sends everything behind it to the backlog too. The
     * ring would be empty, the backlog full, and both sides asleep.
     * So the room is re-checked HERE, after everything has been
     * published, and re-checking is what makes it airtight: for the
     * caller to have missed the flag, its advance must have happened
     * before the flag was set - which is before this check - so this
     * check sees the space it freed. */
    if (st->backlog.n != 0 &&
        (slipstream_load(&st->cq_tail, slipstream_mo_relaxed) -
         slipstream_load(&st->cq_head, slipstream_mo_acquire)) < SLIPSTREAM_CQ_DEPTH) {
      mtx_unlock(&st->mtx);
      continue;
    }
    /* Idle is published INSIDE the handoff lock, and io_uring_submit
     * clears it inside the same lock, so a waiter can never read
     * "nothing can complete" while its own submission is in flight. */
    slipstream_store(&st->engine_idle,
                     st->waiting.n == 0 && st->backlog.n == 0 && st->work_inflight == 0 &&
                         st->queue_head == st->queue.n,
                     slipstream_mo_release);
    mtx_unlock(&st->mtx);

    rc = select(nfds, &rset, &wset, NULL, NULL);
    slipstream_store(&st->engine_idle, 0, slipstream_mo_release);
    slipstream_drain_ctl(st);
    slipstream_reap_work(st); /* results the pool handed back while we slept */
    if (rc > 0) slipstream_drain_ready(st, &rset, &wset);
    else if (rc < 0 && errno != EINTR) slipstream_sweep_dead(st);
    slipstream_flush_backlog(st);
    slipstream_arm_cq_pipe(st);
  }
  slipstream_sqeq_free(&batch);
  return 0;
}

/* CALLER. Hand one errand to the engine and wait for it. Before the
 * engine exists there is nobody to race with, so it runs right here. */
static inline int slipstream_run_on_engine(struct io_uring *st, struct slipstream_errand *e) {
  e->done = 0;
  e->rc = 0;
  if (!st->engine_live) {
    slipstream_run_errand(st, e);
    return e->rc;
  }
  cnd_init(&e->cv);
  mtx_lock(&st->mtx);
  st->errand = e;
  slipstream_store(&st->engine_idle, 0, slipstream_mo_release);
  mtx_unlock(&st->mtx);
  slipstream_poke(st);
  mtx_lock(&st->mtx);
  while (!e->done) cnd_wait(&e->cv, &st->mtx);
  mtx_unlock(&st->mtx);
  cnd_destroy(&e->cv);
  return e->rc;
}

/* The absolute deadline for a __kernel_timespec relative wait, on
 * CLOCK_MONOTONIC so a wall-clock step never shortens or lengthens it. */
static inline struct timespec slipstream_deadline_from(const struct __kernel_timespec *ts) {
  struct timespec d;
  clock_gettime(CLOCK_MONOTONIC, &d);
  d.tv_sec += (time_t)ts->tv_sec;
  d.tv_nsec += (long)ts->tv_nsec;
  if (d.tv_nsec >= 1000000000L) {
    d.tv_nsec -= 1000000000L;
    d.tv_sec += 1;
  }
  return d;
}

/* Milliseconds left, ROUNDED UP: a remainder under a millisecond still
 * has to be a sleep, or the wait would spin against its own deadline. */
static inline int slipstream_remaining_ms(const struct timespec *deadline) {
  struct timespec now;
  int64_t ns, ms;
  clock_gettime(CLOCK_MONOTONIC, &now);
  ns = ((int64_t)(deadline->tv_sec - now.tv_sec) * 1000000000LL) +
       (deadline->tv_nsec - now.tv_nsec);
  if (ns <= 0) return 0;
  ms = (ns + 999999LL) / 1000000LL;
  return ms > INT_MAX ? INT_MAX : (int)ms;
}

/* CALLER. Block on ring_fd. ONE mechanism for the embedder's own poll
 * and for the waits below - a second one would be a second thing to
 * keep in step with the pipe invariant. */
static inline void slipstream_wait_pipe(struct io_uring *st, int timeout_ms) {
  struct pollfd p;
  p.fd = st->ring_fd;
  p.events = POLLIN;
  p.revents = 0;
  poll(&p, 1, timeout_ms);
}

/* ---- setup, registration, buffer ring ------------------------------- */

/* The fields a caller READS back are here too, and they are read back
 * for a reason: the kernel is free to give a different ring than the
 * one asked for, so code that sizes anything by its ring asks the ring
 * afterwards. This implementation is free to answer as well - its
 * depths are fixed, so what it gives is what it always gives, and the
 * caller learns that instead of believing it got what it requested. */
struct io_uring_params {
  unsigned sq_entries;
  unsigned cq_entries;
  unsigned flags;
  unsigned features;
};

/* The engine is born here and joined in io_uring_queue_exit. This is C:
 * there is no constructor and no destructor, so THIS call initialises
 * the struct - it does not inspect it, and a struct that has been
 * through it must go through queue_exit before it goes out of scope, or
 * a thread outlives the memory it points into. Every flag in params
 * describes a discipline this implementation already has. */
static inline int io_uring_queue_init_params(unsigned entries, struct io_uring *st,
                                             struct io_uring_params *p) {
  int cq[2], ctl[2], i;
  (void)entries;

  st->features = 0;
  st->sq = NULL;
  st->sq_n = 0;
  st->cqes = NULL;
  st->ring_fd = st->cq_w = st->ctl_r = st->ctl_w = -1;
  st->errand = NULL;
  st->stopping = 0;
  st->engine_live = 0;
  st->queue_head = 0;
  st->chain_failed = 0;
  st->blocked = 0;
  st->work_inflight = 0;
  st->wq_head = st->wq_tail = NULL;
  st->done_head = st->done_tail = NULL;
  st->wq_idle = 0;
  st->wq_stopping = 0;
  st->workers_n = 0;
  memset(&st->queue, 0, sizeof(st->queue));
  st->files = NULL;
  st->files_n = 0;
  st->free_slots = NULL;
  st->free_n = st->free_cap = 0;
  memset(&st->pending, 0, sizeof(st->pending));
  memset(&st->waiting, 0, sizeof(st->waiting));
  memset(&st->backlog, 0, sizeof(st->backlog));
  st->bufring.bufs = NULL;
  st->bufring.mask = 0;
  slipstream_store(&st->bufring.head, 0u, slipstream_mo_relaxed);
  slipstream_store(&st->bufring.tail, 0u, slipstream_mo_relaxed);
  slipstream_store(&st->cq_head, 0u, slipstream_mo_relaxed);
  slipstream_store(&st->cq_tail, 0u, slipstream_mo_relaxed);
  slipstream_store(&st->poll_quiet, 0u, slipstream_mo_relaxed);
  slipstream_store(&st->cq_armed, 0, slipstream_mo_relaxed);
  slipstream_store(&st->cq_overflow, 0, slipstream_mo_relaxed);
  slipstream_store(&st->engine_idle, 0, slipstream_mo_relaxed);

  st->sq = (struct io_uring_sqe *)calloc(SLIPSTREAM_SQ_DEPTH, sizeof(struct io_uring_sqe));
  st->cqes = (struct io_uring_cqe *)calloc(SLIPSTREAM_CQ_DEPTH, sizeof(struct io_uring_cqe));
  if (!st->sq || !st->cqes) {
    free(st->sq);
    free(st->cqes);
    st->sq = NULL;
    st->cqes = NULL;
    return -ENOMEM;
  }
  if (pipe(cq) != 0) {
    free(st->sq);
    free(st->cqes);
    st->sq = NULL;
    st->cqes = NULL;
    return -errno;
  }
  if (pipe(ctl) != 0) {
    const int e = errno;
    close(cq[0]);
    close(cq[1]);
    free(st->sq);
    free(st->cqes);
    st->sq = NULL;
    st->cqes = NULL;
    return -e;
  }
  st->ring_fd = cq[0];
  st->cq_w = cq[1];
  st->ctl_r = ctl[0];
  st->ctl_w = ctl[1];
  for (i = 0; i < 4; i++) {
    /* Non-blocking on BOTH ends of both pipes: neither side may ever
     * block on the other. And the published ceiling applies to these
     * descriptors exactly as it does to a connection's - the engine puts
     * ctl_r in an fd_set. */
    const int fd = (i == 0 ? st->ring_fd : i == 1 ? st->cq_w : i == 2 ? st->ctl_r : st->ctl_w);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    fcntl(fd, F_SETFD, fcntl(fd, F_GETFD, 0) | FD_CLOEXEC);
    if (fd >= IO_URING_FD_CEILING) {
      close(st->ring_fd);
      close(st->cq_w);
      close(st->ctl_r);
      close(st->ctl_w);
      st->ring_fd = st->cq_w = st->ctl_r = st->ctl_w = -1;
      free(st->sq);
      free(st->cqes);
      st->sq = NULL;
      st->cqes = NULL;
      return -EMFILE;
    }
  }
  if (mtx_init(&st->wq_mtx, mtx_plain) != thrd_success ||
      cnd_init(&st->wq_cv) != thrd_success) {
    close(st->ring_fd);
    close(st->cq_w);
    close(st->ctl_r);
    close(st->ctl_w);
    st->ring_fd = st->cq_w = st->ctl_r = st->ctl_w = -1;
    free(st->sq);
    free(st->cqes);
    st->sq = NULL;
    st->cqes = NULL;
    return -ENOMEM;
  }
  if (mtx_init(&st->mtx, mtx_plain) != thrd_success) {
    close(st->ring_fd);
    close(st->cq_w);
    close(st->ctl_r);
    close(st->ctl_w);
    st->ring_fd = st->cq_w = st->ctl_r = st->ctl_w = -1;
    free(st->sq);
    free(st->cqes);
    st->sq = NULL;
    st->cqes = NULL;
    return -ENOMEM;
  }
  if (mtx_init(&st->pipe_mtx, mtx_plain) != thrd_success) {
    mtx_destroy(&st->mtx);
    close(st->ring_fd);
    close(st->cq_w);
    close(st->ctl_r);
    close(st->ctl_w);
    st->ring_fd = st->cq_w = st->ctl_r = st->ctl_w = -1;
    free(st->sq);
    free(st->cqes);
    st->sq = NULL;
    st->cqes = NULL;
    return -ENOMEM;
  }
  if (thrd_create(&st->engine, slipstream_engine_run, st) != thrd_success) {
    mtx_destroy(&st->mtx);
    mtx_destroy(&st->pipe_mtx);
    close(st->ring_fd);
    close(st->cq_w);
    close(st->ctl_r);
    close(st->ctl_w);
    st->ring_fd = st->cq_w = st->ctl_r = st->ctl_w = -1;
    free(st->sq);
    free(st->cqes);
    st->sq = NULL;
    st->cqes = NULL;
    return -EAGAIN;
  }
  st->engine_live = 1;
  /* Answered LAST, and only on the path that succeeded: a failed init
   * leaves the caller's params untouched, exactly as a failed
   * io_uring_setup does. */
  if (p != NULL) {
    p->sq_entries = SLIPSTREAM_SQ_DEPTH;
    p->cq_entries = SLIPSTREAM_CQ_DEPTH;
    p->features = st->features;
  }
  return 0;
}
static inline int io_uring_queue_init(unsigned entries, struct io_uring *st, unsigned flags) {
  (void)flags;
  return io_uring_queue_init_params(entries, st, NULL);
}

/* Shutdown says so through the control pipe and JOINS. Never detach: the
 * engine holds pointers into this struct, so an engine outliving its
 * ring is a use-after-free with a thread attached to it. Ops still
 * parked are dropped without completions, which is what tearing a ring
 * down means. All of the cleanup is here, and only here - C has no
 * destructor to hide half of it in. */
static inline void io_uring_queue_exit(struct io_uring *st) {
  unsigned i;
  if (st->engine_live) {
    int res = 0;
    mtx_lock(&st->mtx);
    st->stopping = 1;
    mtx_unlock(&st->mtx);
    slipstream_poke(st);
    thrd_join(st->engine, &res);
    /* The engine first: it is the one that would still hand work out.
     * Then the pool, which finishes whatever syscall it is in and drops
     * the rest - see slipstream_worker_run. */
    mtx_lock(&st->wq_mtx);
    st->wq_stopping = 1;
    mtx_unlock(&st->wq_mtx);
    cnd_broadcast(&st->wq_cv);
    for (i = 0; i < st->workers_n; i++) thrd_join(st->workers[i], &res);
    st->workers_n = 0;
    while (st->wq_head != NULL) {
      struct slipstream_work *w = st->wq_head;
      st->wq_head = w->next;
      free(w);
    }
    while (st->done_head != NULL) {
      struct slipstream_work *w = st->done_head;
      st->done_head = w->next;
      free(w);
    }
    st->wq_tail = st->done_tail = NULL;
    mtx_destroy(&st->wq_mtx);
    cnd_destroy(&st->wq_cv);
    mtx_destroy(&st->mtx);
    mtx_destroy(&st->pipe_mtx);
    st->engine_live = 0;
  }
  if (st->ring_fd >= 0) close(st->ring_fd);
  if (st->cq_w >= 0) close(st->cq_w);
  if (st->ctl_r >= 0) close(st->ctl_r);
  if (st->ctl_w >= 0) close(st->ctl_w);
  st->ring_fd = st->cq_w = st->ctl_r = st->ctl_w = -1;
  for (i = 0; i < st->files_n; i++) {
    if (st->files[i] >= 0) close(st->files[i]);
  }
  free(st->files);
  st->files = NULL;
  st->files_n = 0;
  free(st->free_slots);
  st->free_slots = NULL;
  st->free_n = st->free_cap = 0;
  free(st->bufring.bufs);
  st->bufring.bufs = NULL;
  st->bufring.mask = 0;
  slipstream_sqeq_free(&st->pending);
  slipstream_sqeq_free(&st->queue);
  st->queue_head = 0;
  slipstream_sqeq_free(&st->waiting);
  slipstream_cqq_free(&st->backlog);
  free(st->sq);
  st->sq = NULL;
  st->sq_n = 0;
  free(st->cqes);
  st->cqes = NULL;
}

static inline int io_uring_register_ring_fd(struct io_uring *st) {
  (void)st;
  return 0;
}

/* There is no ring to probe and no opcode this file does not implement:
 * what it answers "yes" to is exactly what it can run. */
static inline struct io_uring_probe *io_uring_get_probe(void) {
  static struct io_uring_probe p = {0};
  return &p;
}
static inline void io_uring_free_probe(struct io_uring_probe *p) { (void)p; }
static inline int io_uring_opcode_supported(const struct io_uring_probe *p, int op) {
  (void)p;
  (void)op;
  return 1;
}

/* Registration mutates the direct table the engine resolves fds
 * through, so it is an errand ON the engine, run between two select
 * passes and waited for. Synchronous to the caller, exactly as before. */
static inline int io_uring_register_files_sparse(struct io_uring *st, unsigned n) {
  struct slipstream_errand e;
  memset(&e, 0, sizeof(e));
  e.kind = SLIPSTREAM_ERRAND_FILES_SPARSE;
  e.a = n;
  return slipstream_run_on_engine(st, &e);
}
static inline int io_uring_register_file_alloc_range(struct io_uring *st, unsigned lo, unsigned n) {
  struct slipstream_errand e;
  memset(&e, 0, sizeof(e));
  e.kind = SLIPSTREAM_ERRAND_ALLOC_RANGE;
  e.a = lo;
  e.b = n;
  return slipstream_run_on_engine(st, &e);
}
static inline int io_uring_register_files_update(struct io_uring *st, unsigned off, int *fds,
                                                 unsigned n) {
  struct slipstream_errand e;
  memset(&e, 0, sizeof(e));
  e.kind = SLIPSTREAM_ERRAND_FILES_UPDATE;
  e.a = off;
  e.b = n;
  e.fds = fds;
  return slipstream_run_on_engine(st, &e);
}

static inline struct io_uring_buf_ring *io_uring_setup_buf_ring(struct io_uring *st, unsigned count,
                                                                int bgid, unsigned flags,
                                                                int *err) {
  struct slipstream_errand e;
  int rc;
  (void)bgid;
  (void)flags;
  memset(&e, 0, sizeof(e));
  e.kind = SLIPSTREAM_ERRAND_SETUP_BUFRING;
  e.a = count;
  rc = slipstream_run_on_engine(st, &e);
  if (err) *err = rc;
  return rc == 0 ? &st->bufring : NULL;
}
static inline void io_uring_free_buf_ring(struct io_uring *st, struct io_uring_buf_ring *br,
                                          unsigned count, int bgid) {
  (void)st;
  (void)br;
  (void)count;
  (void)bgid;
}
static inline int io_uring_buf_ring_mask(unsigned count) { return (int)(count - 1u); }
static inline void io_uring_buf_ring_add(struct io_uring_buf_ring *br, void *addr, unsigned len,
                                         uint16_t bid, int mask, int off) {
  struct io_uring_buf *e =
      &br->bufs[(slipstream_load(&br->tail, slipstream_mo_relaxed) + (uint32_t)off) &
                (uint32_t)mask];
  e->addr = addr;
  e->len = len;
  e->bid = bid;
}
static inline void io_uring_buf_ring_advance(struct io_uring_buf_ring *br, int n) {
  /* Release: this is the publish. Everything the buf_ring_add calls
   * above wrote becomes visible to the engine with this store, and not
   * before. */
  slipstream_store(&br->tail, slipstream_load(&br->tail, slipstream_mo_relaxed) + (uint32_t)n,
                   slipstream_mo_release);
}

/* ---- the queue ------------------------------------------------------ */

static inline struct io_uring_sqe *io_uring_get_sqe(struct io_uring *st) {
  struct io_uring_sqe *s;
  if (st->sq_n >= SLIPSTREAM_SQ_DEPTH) return NULL; /* the caller submits and retries */
  s = &st->sq[st->sq_n++];
  memset(s, 0, sizeof(*s));
  return s;
}
static inline unsigned io_uring_sq_space_left(const struct io_uring *st) {
  return SLIPSTREAM_SQ_DEPTH - st->sq_n;
}
static inline int io_uring_peek_cqe(struct io_uring *st, struct io_uring_cqe **cqe) {
  if (slipstream_cq_ready(st) == 0) return -EAGAIN;
  *cqe = &st->cqes[slipstream_load(&st->cq_head, slipstream_mo_relaxed) & SLIPSTREAM_CQ_MASK];
  return 0;
}

/* The batch walk, in liburing's own macro form so caller code compiles
 * unchanged against either implementation. `head` is the caller's loop
 * variable, not this struct's state - it walks the ring without
 * consuming any of it, exactly like liburing's khead/ktail walk leaves
 * the head alone until cq_advance moves it, and it is free-running, so
 * it wraps the way the kernel's does. The acquire load of the tail on
 * every step is what makes this safe with no lock at all: the engine
 * publishes a completion by releasing the tail past it, and never writes
 * a slot below it again. `ring` is a pointer, as it is everywhere else
 * in this header. */
#define io_uring_for_each_cqe(ring, head, cqe)                                          \
  for ((head) = slipstream_load(&(ring)->cq_head, slipstream_mo_relaxed);               \
       ((cqe) = ((head) != slipstream_load(&(ring)->cq_tail, slipstream_mo_acquire)     \
                     ? &(ring)->cqes[(head) & SLIPSTREAM_CQ_MASK]                       \
                     : NULL)) != NULL;                                                  \
       (head)++)

/* Must be called after io_uring_for_each_cqe(), exactly liburing's
 * contract - and PARTIAL by construction: nr may be less than what the
 * walk just saw, and whatever is left simply stays in the ring for the
 * next tick. That is the reason this exists: a tick cut off mid-batch
 * (webmachine's budgeted Webmachine.tick, #116) must not drop the
 * remainder. nr beyond what for_each_cqe exposed is caller error the
 * same as it would be against liburing (there head/tail would desync);
 * clamped here only so head cannot walk past tail.
 *
 * This is also the caller's half of the ring_fd invariant, and the only
 * place it lives: emptying the ring drinks the pipe. And it is where an
 * overflowed engine is told there is room again - the backlog is the
 * engine's, so the caller pokes rather than touching it. */
static inline void io_uring_cq_advance(struct io_uring *st, unsigned nr) {
  const unsigned head = slipstream_load(&st->cq_head, slipstream_mo_relaxed);
  const unsigned avail = slipstream_load(&st->cq_tail, slipstream_mo_acquire) - head;
  const unsigned n = nr < avail ? nr : avail;
  if (n == 0) return;
  /* Release: the engine may reuse these slots once it sees this, and must
   * not see it before the walk above finished reading them. */
  slipstream_store(&st->cq_head, head + n, slipstream_mo_release);
  slipstream_drain_cq_pipe(st);
  if (slipstream_load(&st->cq_overflow, slipstream_mo_relaxed)) slipstream_poke(st);
  /* A quiet multishot poll is waiting for exactly this: its completion
   * has now been taken, so it may speak again. The store above is a
   * release and this load an acquire, so the engine cannot see the poke
   * without also seeing the new head - which is what makes a lost
   * wakeup impossible. Costs a write on the control pipe only while a
   * poll is actually quiet, which is only while a caller is behind. */
  if (slipstream_load(&st->poll_quiet, slipstream_mo_acquire)) slipstream_poke(st);
}

static inline void io_uring_cqe_seen(struct io_uring *st, struct io_uring_cqe *cqe) {
  if (cqe) io_uring_cq_advance(st, 1);
}

/* Pure handoff: the prepped SQEs go to the engine in submission order,
 * and the count comes back, which is all submit ever promised. Nothing
 * is executed here - the completion for a NOP is not guaranteed to
 * exist by the time this returns, any more than it is against a real
 * ring. Code that read a completion straight after submit was relying
 * on an accident of the inline implementation; the wait functions below
 * are the contract. */
static inline int io_uring_submit(struct io_uring *st) {
  const unsigned n = st->sq_n;
  int ok;
  if (n == 0) return 0;
  mtx_lock(&st->mtx);
  ok = slipstream_sqeq_append(&st->pending, st->sq, n);
  /* Cleared under the same lock the engine publishes idle under, so a
   * waiter can never mistake in-flight work for an idle engine. */
  slipstream_store(&st->engine_idle, 0, slipstream_mo_release);
  mtx_unlock(&st->mtx);
  st->sq_n = 0;
  slipstream_poke(st);
  return ok ? (int)n : -ENOMEM;
}

/* Wait until the ring holds wait_nr completions, with no deadline.
 *
 * The one way out other than completions: the engine reports itself idle
 * - nothing submitted, nothing armed, nothing backed up - which means no
 * completion can ever arrive, and blocking would be a hang. The inline
 * implementation gave the same answer by finding its waiting set empty
 * ("nothing armed: do not spin"); this is that rule, moved to where the
 * knowledge now lives. It is safe because every accepted SQE produces at
 * least one completion: if the engine went idle it either published
 * something - and then the pipe is armed and the poll returns at once -
 * or there was nothing to publish. */
static inline int io_uring_submit_and_wait(struct io_uring *st, unsigned wait_nr) {
  const int n = io_uring_submit(st);
  while (slipstream_cq_ready(st) < wait_nr) {
    if (slipstream_load(&st->engine_idle, slipstream_mo_acquire)) break;
    slipstream_wait_pipe(st, -1);
  }
  return n;
}

/* liburing's own name for "wait for one completion and hand it to me".
 * -EAGAIN when nothing can arrive at all, which is the only honest
 * answer once the engine has reported itself idle. */
static inline int io_uring_wait_cqe(struct io_uring *st, struct io_uring_cqe **cqe_ptr) {
  while (slipstream_cq_ready(st) == 0) {
    if (slipstream_load(&st->engine_idle, slipstream_mo_acquire)) {
      if (slipstream_cq_ready(st) == 0) {
        if (cqe_ptr) *cqe_ptr = NULL;
        return -EAGAIN;
      }
      break;
    }
    slipstream_wait_pipe(st, -1);
  }
  return io_uring_peek_cqe(st, cqe_ptr);
}

/* Wait with a deadline: the point of this one over submit_and_wait is
 * that the caller can cap how long a tick may block instead of asking
 * for that in its own way - here ts becomes poll's timeout on ring_fd,
 * the same descriptor an embedder would poll itself.
 *
 * ts == NULL means what it means in liburing: no deadline at all, so
 * this degrades to exactly submit_and_wait (only wait_nr governs, and
 * -ETIME can never happen - there is no timeout to run out).
 *
 * With a real ts, running out before wait_nr completions have arrived
 * returns -ETIME, liburing's own documented behaviour for this function.
 * An empty ring is NOT a shortcut past the wait: with nothing armed the
 * deadline is still slept out, the same as it would be against a real
 * ring with an outstanding but slow request - so the idle escape above
 * deliberately does not apply here, since the deadline already
 * guarantees this returns.
 *
 * sigmask is accepted for signature parity; nothing here can be
 * interrupted by a blocked signal the way a real io_uring_enter can, so
 * it does nothing. */
static inline int io_uring_submit_and_wait_timeout(struct io_uring *st,
                                                   struct io_uring_cqe **cqe_ptr, unsigned wait_nr,
                                                   struct __kernel_timespec *ts, sigset_t *sig) {
  const int n = io_uring_submit(st);
  (void)sig;

  if (ts == NULL) {
    while (slipstream_cq_ready(st) < wait_nr) {
      if (slipstream_load(&st->engine_idle, slipstream_mo_acquire)) break;
      slipstream_wait_pipe(st, -1);
    }
  } else {
    const struct timespec deadline = slipstream_deadline_from(ts);
    while (slipstream_cq_ready(st) < wait_nr) {
      const int ms = slipstream_remaining_ms(&deadline);
      if (ms == 0) {
        if (cqe_ptr) *cqe_ptr = NULL;
        return -ETIME;
      }
      slipstream_wait_pipe(st, ms);
    }
  }

  if (cqe_ptr) {
    *cqe_ptr = slipstream_cq_ready(st) == 0
                   ? NULL
                   : &st->cqes[slipstream_load(&st->cq_head, slipstream_mo_relaxed) &
                               SLIPSTREAM_CQ_MASK];
  }
  return n;
}

#endif
