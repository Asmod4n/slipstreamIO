// slipstreamIO: the io_uring submission/completion API, run on
// select(2).
//
// This file is named for the API it implements, because that is the
// only name anyone ever writes. It does NOT sit on an include path by
// itself - mrbgem.rake copies it into include/ exactly when this host
// has no other liburing.h, and leaves it here otherwise.
//
// WHAT THIS IS: an implementation of that API's shape - submission
// queue entries in, completion queue entries out - for machines where
// the real thing is not available. It knows NOTHING about the library
// it stands in for: it never probes for it, never includes it, never
// decides anything. It is one implementation, and choosing it is
// somebody else's job.
//
// WHERE THE CHOICE IS MADE, and deliberately not here: at the packaging
// layer, the way libkqueue is chosen on Linux. A consumer writes
// `#include <liburing.h>` and calls the functions; whoever assembles
// the build decides which implementation that resolves to - by
// installing this header under that name, or by not installing it.
// Source code never asks, so there is no __has_include, no define to
// keep in sync, no template parameter and no runtime branch anywhere
// above this file. The binary also stays statically linkable: there is
// nothing to dlopen, which matters because a static dlopen fails on
// glibc and musl alike.
//
// DESIGN GOAL, explicit: this is CORRECT, not fast. Laziness is the
// declared motive. Nobody optimizes it later.
//   - every operation is readiness + a classic syscall
//   - submission does the work inline: an op that can answer now
//     answers inside submit, the rest parks until select says ready
//   - recv bundles do not exist; one buffer per completion, so the
//     dense-fill contract holds trivially
//   - file IO is absent on purpose. select on a regular file always
//     says ready, so a file read would run synchronously and block the
//     caller's loop. Nothing here opens one.
//   - capacity is capped below FD_SETSIZE by whoever sets the limit:
//     here a connection IS a process fd. That ceiling is PUBLISHED, as
//     IO_URING_FD_CEILING below, so a consumer reads the property
//     instead of guessing it from this implementation's name
//
// WHY select AND NOT poll/kqueue/epoll: it is the one readiness
// primitive that exists everywhere AND has been debugged everywhere -
// macOS' poll is permanently broken on several fd types, WSAPoll does
// not report failed connections (acknowledged by Microsoft, never
// fixed), kqueue is BSD-only, epoll Linux-only. One primitive means ONE
// implementation instead of three with three test matrices. The
// operations below are still Linux syscalls; the SHAPE is what is
// portable, and no line here pretends the port has been done.
#ifndef SLIPSTREAM_IO_H
#define SLIPSTREAM_IO_H

// The name of this implementation, and NOTHING that follows from it.
// It exists so a build can SAY which implementation answered - the
// startup banner that reports "correct, not fast" is the whole use
// case. No limit and no behaviour may be derived from it: a name is
// not a property, and every implementation of this API that comes
// later would inherit whatever was hung on this one's name.
#define SLIPSTREAM_IO 1

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <deque>
#include <vector>

// __kernel_timespec: liburing gets this from <linux/time_types.h>
// (through its own compat.h), so use the real thing wherever it is
// available - not just on Linux, in case some other host ships it too.
// Guarded with the kernel header's own include guard, so this fallback
// steps aside the moment the real header is reachable, and a later
// include of it (directly, or pulled in by something else) never
// collides with a struct already defined here. Where neither happens -
// the case this exists for - the shape is the kernel's own: two 64-bit
// fields, nothing else, so a consumer that reads tv_sec/tv_nsec sees the
// same layout it would from the real header.
#if __has_include(<linux/time_types.h>)
#include <linux/time_types.h>
#elif !defined(_LINUX_TIME_TYPES_H)
#define _LINUX_TIME_TYPES_H
struct __kernel_timespec {
  int64_t tv_sec;
  int64_t tv_nsec;
};
#endif

// ---- the property a consumer may branch on --------------------------
//
// IO_URING_FD_CEILING: every descriptor handed to these functions must
// be strictly below this number. It is stated because it is TRUE here
// and for no other reason - select(2) addresses FD_SETSIZE descriptors
// and nothing above, and a consumer whose connections are process fds
// has to keep its own rlimit under the same roof or hand this API an
// fd it cannot put in an fd_set.
//
// ABSENCE is the other half of the contract, and the important half:
// an implementation with no ceiling of its own defines nothing, and
// real liburing never will. A consumer that finds no ceiling has been
// told there is none - its rlimits are the only bound. So the question
// to ask is `#ifdef IO_URING_FD_CEILING`, never `#ifdef SLIPSTREAM_IO`:
// the second asks WHO answered and gets select's ceiling handed to
// every future answer, including IOCP (no fd_set at all) and a macOS
// build with _DARWIN_UNLIMITED_SELECT (heap fd_sets, a different
// number). Written down while there is exactly one consumer, because
// that is when it is cheap.
//
// It sits below the includes because FD_SETSIZE is <sys/select.h>'s to
// define, and this file only reports it.
#define IO_URING_FD_CEILING FD_SETSIZE

// ---- the ABI names, as far as this tree uses them -------------------
//
// These mirror liburing's spelling, NOT the kernel's layout: in a build
// without liburing nothing else in the process shares these structs, so
// they only have to carry what the prep_* writers below record and what
// submit() reads back. Field names match liburing's so that ring.hpp's
// direct writes (s->flags |= IOSQE_FIXED_FILE) compile unchanged.

enum {
  IORING_OP_NOP = 0,
  IORING_OP_ACCEPT,
  IORING_OP_BIND,
  IORING_OP_CLOSE,
  IORING_OP_LISTEN,
  IORING_OP_POLL_ADD,
  IORING_OP_RECV,
  IORING_OP_SEND,
  IORING_OP_SENDMSG,
  IORING_OP_SHUTDOWN,
  IORING_OP_SOCKET,
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
#define IORING_CQE_BUFFER_SHIFT 16
#define IORING_MAX_FIXED_FILES (1u << 20)
#define IORING_RECVSEND_BUNDLE (1u << 4)

// Setup flags the Ring asks for. They describe a submission-queue
// discipline this implementation has by construction - one thread, one
// caller, work done inline in submit - so they are accepted and mean
// nothing here.
enum {
  IORING_SETUP_SINGLE_ISSUER = 1u << 12,
  IORING_SETUP_DEFER_TASKRUN = 1u << 13,
  IORING_SETUP_COOP_TASKRUN = 1u << 8
};
// Deliberately NOT set in io_uring::features: recv bundles do not exist
// here, so the Ring reads bundles_ = false and takes one buffer per
// completion.
#define IORING_FEAT_RECVSEND_BUNDLE (1u << 14)

#ifndef SOCKET_URING_OP_SETSOCKOPT
#define SOCKET_URING_OP_SETSOCKOPT 1
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
  uint32_t unlink_flags;
  uint32_t file_index;
  uint16_t buf_group;
  uint32_t cmd_op;
  uint32_t level;
  uint32_t optname;
  uint64_t optval;
  uint32_t optlen;
  uint64_t user_data;
};

struct io_uring_cqe {
  uint64_t user_data;
  int32_t res;
  uint32_t flags;
};

struct io_uring_buf_ring {
  struct Ent {
    void* addr = nullptr;
    unsigned len = 0;
    uint16_t bid = 0;
  };
  std::vector<Ent> ents;
  uint32_t mask = 0;
  // Free-running cursors, exactly the kernel's shape: the provider
  // advances tail, consumption walks head strictly in ring order - the
  // advance-only replenish contract the Ring relies on.
  uint32_t head = 0;
  uint32_t tail = 0;
};

struct io_uring {
  static constexpr unsigned kSqDepth = 1024;
  unsigned features = 0;  // no IORING_FEAT_* - bundles must read false
  // Real liburing's struct carries the ring's own pollable fd here;
  // POLLIN on it means "completions are waiting" and is the canonical
  // way to hang a ring into a foreign event loop. This implementation
  // CANNOT offer that, structurally: completions materialize only
  // inside API calls (submit walks the parked set), so no kernel-side
  // object ever aggregates readiness into one fd - and signalling one
  // ourselves (io_uring_register_eventfd's job) would take a thread,
  // which "no loop, no thread, no lifetime" rules out. -1 is the
  // in-band property: a consumer that wants to export a pollable fd
  // checks ring_fd >= 0 and refuses by name otherwise, pointing at
  // the bounded wait (io_uring_submit_and_wait_timeout) instead -
  // which select serves natively. Same answer IOCP will give: a
  // completion port is waitable, not pollable.
  int ring_fd = -1;
  std::vector<struct io_uring_sqe> sq;  // written by the prep_* below
  std::deque<struct io_uring_cqe> cq;
  std::vector<int> files;            // slot -> fd; the direct table, spelled out
  std::vector<uint32_t> free_slots;  // the alloc range's free list
  uint32_t alloc_lo = 0, alloc_n = 0;
  std::vector<struct io_uring_sqe> waiting;  // armed ops awaiting readiness
  struct io_uring_buf_ring bufring;
  ~io_uring() {
    for (int fd : files) {
      if (fd >= 0) ::close(fd);
    }
  }
};

struct io_uring_probe {
  int unused;
};

// ---- prep: pure struct writers, exactly like liburing's -------------
//
// Not one of these makes a syscall in liburing either; they record what
// the operation is. submit() below is where anything happens.

inline void io_uring_prep_nop(struct io_uring_sqe* s) {
  std::memset(s, 0, sizeof(*s));
  s->opcode = IORING_OP_NOP;
}
inline void io_uring_sqe_set_data64(struct io_uring_sqe* s, uint64_t d) { s->user_data = d; }
inline uint64_t io_uring_cqe_get_data64(const struct io_uring_cqe* c) { return c->user_data; }

inline void io_uring_prep_unlink(struct io_uring_sqe* s, const char* path, int flags) {
  io_uring_prep_nop(s);
  s->opcode = IORING_OP_UNLINKAT;
  s->fd = AT_FDCWD;
  s->addr = reinterpret_cast<uintptr_t>(path);
  s->unlink_flags = static_cast<uint32_t>(flags);
}
inline void io_uring_prep_socket_direct(struct io_uring_sqe* s, int domain, int type, int protocol,
                                        unsigned file_index, unsigned flags) {
  io_uring_prep_nop(s);
  s->opcode = IORING_OP_SOCKET;
  s->fd = domain;
  s->off = static_cast<uint64_t>(type);
  s->len = static_cast<uint32_t>(protocol);
  s->msg_flags = flags;
  s->file_index = file_index + 1;  // liburing's +1 encoding, kept
}
inline void io_uring_prep_cmd_sock(struct io_uring_sqe* s, int cmd_op, int fd, int level,
                                   int optname, void* optval, int optlen) {
  io_uring_prep_nop(s);
  s->opcode = IORING_OP_URING_CMD;
  s->cmd_op = static_cast<uint32_t>(cmd_op);
  s->fd = fd;
  s->level = static_cast<uint32_t>(level);
  s->optname = static_cast<uint32_t>(optname);
  s->optval = reinterpret_cast<uintptr_t>(optval);
  s->optlen = static_cast<uint32_t>(optlen);
}
inline void io_uring_prep_bind(struct io_uring_sqe* s, int fd, struct sockaddr* addr,
                               socklen_t addrlen) {
  io_uring_prep_nop(s);
  s->opcode = IORING_OP_BIND;
  s->fd = fd;
  s->addr = reinterpret_cast<uintptr_t>(addr);
  s->off = addrlen;
}
inline void io_uring_prep_listen(struct io_uring_sqe* s, int fd, int backlog) {
  io_uring_prep_nop(s);
  s->opcode = IORING_OP_LISTEN;
  s->fd = fd;
  s->len = static_cast<uint32_t>(backlog);
}
inline void io_uring_prep_multishot_accept_direct(struct io_uring_sqe* s, int fd,
                                                  struct sockaddr* addr, socklen_t* addrlen,
                                                  int flags) {
  io_uring_prep_nop(s);
  s->opcode = IORING_OP_ACCEPT;
  s->fd = fd;
  s->addr = reinterpret_cast<uintptr_t>(addr);
  s->off = reinterpret_cast<uintptr_t>(addrlen);
  s->msg_flags = static_cast<uint32_t>(flags);
}
inline void io_uring_prep_recv(struct io_uring_sqe* s, int fd, void* buf, size_t len, int flags) {
  io_uring_prep_nop(s);
  s->opcode = IORING_OP_RECV;
  s->fd = fd;
  s->addr = reinterpret_cast<uintptr_t>(buf);
  s->len = static_cast<uint32_t>(len);
  s->msg_flags = static_cast<uint32_t>(flags);
}
inline void io_uring_prep_recv_multishot(struct io_uring_sqe* s, int fd, void* buf, size_t len,
                                         int flags) {
  io_uring_prep_recv(s, fd, buf, len, flags);
}
inline void io_uring_prep_send(struct io_uring_sqe* s, int fd, const void* buf, size_t len,
                               int flags) {
  io_uring_prep_nop(s);
  s->opcode = IORING_OP_SEND;
  s->fd = fd;
  s->addr = reinterpret_cast<uintptr_t>(buf);
  s->len = static_cast<uint32_t>(len);
  s->msg_flags = static_cast<uint32_t>(flags);
}
inline void io_uring_prep_sendmsg(struct io_uring_sqe* s, int fd, const struct msghdr* msg,
                                  unsigned flags) {
  io_uring_prep_nop(s);
  s->opcode = IORING_OP_SENDMSG;
  s->fd = fd;
  s->addr = reinterpret_cast<uintptr_t>(msg);
  s->len = 1;
  s->msg_flags = flags;
}
inline void io_uring_prep_shutdown(struct io_uring_sqe* s, int fd, int how) {
  io_uring_prep_nop(s);
  s->opcode = IORING_OP_SHUTDOWN;
  s->fd = fd;
  s->len = static_cast<uint32_t>(how);
}
inline void io_uring_prep_close_direct(struct io_uring_sqe* s, unsigned file_index) {
  io_uring_prep_nop(s);
  s->opcode = IORING_OP_CLOSE;
  s->file_index = file_index + 1;
}
inline void io_uring_prep_poll_add(struct io_uring_sqe* s, int fd, unsigned poll_mask) {
  io_uring_prep_nop(s);
  s->opcode = IORING_OP_POLL_ADD;
  s->fd = fd;
  s->len = poll_mask;
}

// ---- setup, registration, buffer ring -------------------------------

struct io_uring_params {
  unsigned flags;
};

inline int io_uring_queue_init_params(unsigned, struct io_uring*, struct io_uring_params*) {
  return 0;
}
inline int io_uring_queue_init(unsigned, struct io_uring*, unsigned) { return 0; }
inline void io_uring_queue_exit(struct io_uring*) {}  // the destructor closes what remains
inline int io_uring_register_ring_fd(struct io_uring*) { return 0; }

// There is no ring to probe and no opcode this file does not implement:
// what it answers "yes" to is exactly what it can run.
inline struct io_uring_probe* io_uring_get_probe(void) {
  static struct io_uring_probe p{0};
  return &p;
}
inline void io_uring_free_probe(struct io_uring_probe*) {}
inline bool io_uring_opcode_supported(const struct io_uring_probe*, int) { return true; }

inline int io_uring_register_files_sparse(struct io_uring* st, unsigned n) {
  st->files.assign(n, -1);
  return 0;
}
inline int io_uring_register_file_alloc_range(struct io_uring* st, unsigned lo, unsigned n) {
  st->alloc_lo = lo;
  st->alloc_n = n;
  st->free_slots.clear();
  st->free_slots.reserve(n);
  // Descending, so allocation hands out low slots first (cosmetic
  // parity with the kernel; the Ring only checks the bound).
  for (unsigned i = n; i-- > 0;) st->free_slots.push_back(lo + i);
  return 0;
}
inline int io_uring_register_files_update(struct io_uring* st, unsigned off, int* fds, unsigned n) {
  for (unsigned i = 0; i < n; i++) {
    if (off + i >= st->files.size()) return -EINVAL;
    // dup: the caller may close its copy after registering, exactly
    // like the kernel's table taking its own reference.
    st->files[off + i] = ::dup(fds[i]);
  }
  return static_cast<int>(n);
}

inline struct io_uring_buf_ring* io_uring_setup_buf_ring(struct io_uring* st, unsigned count, int,
                                                         unsigned, int*) {
  st->bufring.ents.assign(count, {});
  st->bufring.mask = count - 1;
  st->bufring.head = st->bufring.tail = 0;
  return &st->bufring;
}
inline void io_uring_free_buf_ring(struct io_uring*, struct io_uring_buf_ring*, unsigned, int) {}
inline int io_uring_buf_ring_mask(unsigned count) { return static_cast<int>(count - 1); }
inline void io_uring_buf_ring_add(struct io_uring_buf_ring* br, void* addr, unsigned len,
                                  uint16_t bid, int mask, int off) {
  io_uring_buf_ring::Ent& e =
      br->ents[(br->tail + static_cast<uint32_t>(off)) & static_cast<uint32_t>(mask)];
  e.addr = addr;
  e.len = len;
  e.bid = bid;
}
inline void io_uring_buf_ring_advance(struct io_uring_buf_ring* br, int n) {
  br->tail += static_cast<uint32_t>(n);
}

// ---- the queue ------------------------------------------------------

inline struct io_uring_sqe* io_uring_get_sqe(struct io_uring* st) {
  if (st->sq.size() >= io_uring::kSqDepth) return nullptr;  // the caller submits and retries
  st->sq.emplace_back();
  struct io_uring_sqe* s = &st->sq.back();
  std::memset(s, 0, sizeof(*s));
  return s;
}
inline unsigned io_uring_sq_space_left(const struct io_uring* st) {
  return static_cast<unsigned>(io_uring::kSqDepth - st->sq.size());
}
inline int io_uring_peek_cqe(struct io_uring* st, struct io_uring_cqe** cqe) {
  if (st->cq.empty()) return -EAGAIN;
  *cqe = &st->cq.front();
  return 0;
}

// The batch walk, in liburing's own macro form so caller code compiles
// unchanged against either implementation. `head` is the caller's loop
// variable, not this struct's state - it walks cq without consuming any
// of it, exactly like liburing's khead/ktail walk leaves the kernel-side
// head alone until cq_advance moves it. `ring` is a pointer, as it is
// everywhere else in this header.
#define io_uring_for_each_cqe(ring, head, cqe)                                        \
  for ((head) = 0; ((cqe) = ((head) < (ring)->cq.size() ? &(ring)->cq[head] : nullptr)) != \
                    nullptr;                                                          \
       (head)++)

// Must be called after io_uring_for_each_cqe(), exactly liburing's
// contract - and PARTIAL by construction: nr may be less than what the
// walk just saw, and whatever is left simply stays at the front of cq
// for the next tick. That is the reason task 1 exists: a tick cut off
// mid-batch (webmachine's budgeted Webmachine.tick, #116) must not drop
// the remainder. nr beyond what for_each_cqe exposed is caller error the
// same as it would be against liburing (there head/tail would desync);
// clamped here only so it cannot walk this deque past empty.
inline void io_uring_cq_advance(struct io_uring* st, unsigned nr) {
  const unsigned n = nr < st->cq.size() ? nr : static_cast<unsigned>(st->cq.size());
  for (unsigned i = 0; i < n; i++) st->cq.pop_front();
}

inline void io_uring_cqe_seen(struct io_uring* st, struct io_uring_cqe* cqe) {
  if (cqe) io_uring_cq_advance(st, 1);
}

namespace slipstream::detail {

inline void push_cqe(struct io_uring* st, uint64_t ud, int32_t res, uint32_t flags) {
  struct io_uring_cqe c {};
  c.user_data = ud;
  c.res = res;
  c.flags = flags;
  st->cq.push_back(c);
}

inline bool deferred(uint8_t op) {
  return op == IORING_OP_ACCEPT || op == IORING_OP_RECV || op == IORING_OP_SEND ||
         op == IORING_OP_SENDMSG || op == IORING_OP_POLL_ADD;
}

inline int resolve_fd(struct io_uring* st, const struct io_uring_sqe& s) {
  if (s.flags & IOSQE_FIXED_FILE) {
    const uint32_t slot = static_cast<uint32_t>(s.fd);
    if (slot >= st->files.size()) return -1;
    return st->files[slot];
  }
  return s.fd;
}

// The ops that answer immediately - each decoded from the fields the
// prep_* writer above recorded.
inline int execute(struct io_uring* st, struct io_uring_sqe& s) {
  switch (s.opcode) {
    case IORING_OP_NOP:
      return 0;
    case IORING_OP_UNLINKAT: {
      const char* path = reinterpret_cast<const char*>(static_cast<uintptr_t>(s.addr));
      return ::unlinkat(s.fd, path, static_cast<int>(s.unlink_flags)) == 0 ? 0 : -errno;
    }
    case IORING_OP_SOCKET: {
      const int fd = ::socket(s.fd, static_cast<int>(s.off) | SOCK_NONBLOCK | SOCK_CLOEXEC,
                              static_cast<int>(s.len));
      if (fd < 0) return -errno;
      if (fd >= IO_URING_FD_CEILING) {  // the assertion that can structurally never fire
        ::close(fd);
        return -EMFILE;
      }
      const uint32_t slot = s.file_index - 1;
      if (slot >= st->files.size()) {
        ::close(fd);
        return -EINVAL;
      }
      st->files[slot] = fd;
      return 0;
    }
    case IORING_OP_URING_CMD: {
      if (s.cmd_op != SOCKET_URING_OP_SETSOCKOPT) return -EOPNOTSUPP;
      const int fd = resolve_fd(st, s);
      if (fd < 0) return -EBADF;
      const void* val = reinterpret_cast<const void*>(static_cast<uintptr_t>(s.optval));
      return ::setsockopt(fd, static_cast<int>(s.level), static_cast<int>(s.optname), val,
                          s.optlen) == 0
                 ? 0
                 : -errno;
    }
    case IORING_OP_BIND: {
      const int fd = resolve_fd(st, s);
      if (fd < 0) return -EBADF;
      const struct sockaddr* sa =
          reinterpret_cast<const struct sockaddr*>(static_cast<uintptr_t>(s.addr));
      return ::bind(fd, sa, static_cast<socklen_t>(s.off)) == 0 ? 0 : -errno;
    }
    case IORING_OP_LISTEN: {
      const int fd = resolve_fd(st, s);
      if (fd < 0) return -EBADF;
      return ::listen(fd, static_cast<int>(s.len)) == 0 ? 0 : -errno;
    }
    case IORING_OP_SHUTDOWN: {
      const int fd = resolve_fd(st, s);
      if (fd < 0) return -EBADF;
      return ::shutdown(fd, static_cast<int>(s.len)) == 0 ? 0 : -errno;
    }
    case IORING_OP_CLOSE: {
      // The slot is file_index-1. Closing also drops whatever was armed
      // on that fd - the kernel would post -ECANCELED, but the Ring's
      // gen guard ignores those anyway; not existing is as good as
      // being ignored.
      const uint32_t slot = s.file_index - 1;
      if (slot >= st->files.size() || st->files[slot] < 0) return -EBADF;
      ::close(st->files[slot]);
      st->files[slot] = -1;
      for (size_t i = 0; i < st->waiting.size();) {
        if (static_cast<uint32_t>(st->waiting[i].fd) == slot &&
            (st->waiting[i].flags & IOSQE_FIXED_FILE) != 0) {
          st->waiting[i] = st->waiting.back();
          st->waiting.pop_back();
        } else {
          i++;
        }
      }
      st->free_slots.push_back(slot);
      return 0;
    }
    default:
      return -EOPNOTSUPP;
  }
}

// Builds the fd_sets for everything armed. Returns the nfds argument
// select(2) wants, or 0 when nothing armed resolves to a live fd - the
// timed and untimed waits below share this, since both hand its result
// to select the same way and only differ in the timeout they pass.
inline int build_waitsets(struct io_uring* st, fd_set* rset, fd_set* wset) {
  FD_ZERO(rset);
  FD_ZERO(wset);
  int nfds = 0;
  for (const struct io_uring_sqe& w : st->waiting) {
    const int fd = resolve_fd(st, w);
    if (fd < 0 || fd >= IO_URING_FD_CEILING) continue;
    if (w.opcode == IORING_OP_SEND || w.opcode == IORING_OP_SENDMSG) FD_SET(fd, wset);
    else FD_SET(fd, rset);
    if (fd + 1 > nfds) nfds = fd + 1;
  }
  return nfds;
}

// Dispatches whatever select(2) found ready into completions - the
// per-opcode handling wait_ready always did, factored out so
// io_uring_submit_and_wait_timeout can run the identical dispatch after
// its own, deadline-bounded select call.
inline void drain_ready(struct io_uring* st, const fd_set& rset, const fd_set& wset) {
  for (size_t i = 0; i < st->waiting.size();) {
    struct io_uring_sqe& w = st->waiting[i];
    const int fd = resolve_fd(st, w);
    bool remove = false;
    if (fd >= 0 && fd < IO_URING_FD_CEILING) {
      if (w.opcode == IORING_OP_SEND && FD_ISSET(fd, &wset)) {
        const void* buf = reinterpret_cast<const void*>(static_cast<uintptr_t>(w.addr));
        const ssize_t r = ::send(fd, buf, w.len, static_cast<int>(w.msg_flags) | MSG_NOSIGNAL);
        push_cqe(st, w.user_data, r >= 0 ? static_cast<int>(r) : -errno, 0);
        remove = true;
      } else if (w.opcode == IORING_OP_SENDMSG && FD_ISSET(fd, &wset)) {
        // The delivery plan (#168): head plus pointers into the asset
        // mapping, handed over as one msghdr. prep_sendmsg puts the
        // msghdr POINTER in addr, and the Ring keeps that msghdr in the
        // connection, so it is still alive when this deferred op runs.
        struct msghdr* m = reinterpret_cast<struct msghdr*>(static_cast<uintptr_t>(w.addr));
        const ssize_t r = ::sendmsg(fd, m, static_cast<int>(w.msg_flags) | MSG_NOSIGNAL);
        push_cqe(st, w.user_data, r >= 0 ? static_cast<int>(r) : -errno, 0);
        remove = true;
      } else if (w.opcode == IORING_OP_ACCEPT && FD_ISSET(fd, &rset)) {
        const int nf = ::accept4(fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (nf >= 0) {
          if (nf >= IO_URING_FD_CEILING || st->free_slots.empty()) {
            ::close(nf);
            push_cqe(st, w.user_data, -ENFILE, 0);  // no MORE: the Ring re-arms
            remove = true;
          } else {
            const uint32_t slot = st->free_slots.back();
            st->free_slots.pop_back();
            st->files[slot] = nf;
            push_cqe(st, w.user_data, static_cast<int>(slot), IORING_CQE_F_MORE);
          }
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
          push_cqe(st, w.user_data, -errno, IORING_CQE_F_MORE);  // transient, stays armed
        }
      } else if (w.opcode == IORING_OP_RECV && FD_ISSET(fd, &rset)) {
        struct io_uring_buf_ring& br = st->bufring;
        if (br.tail == br.head) {
          push_cqe(st, w.user_data, -ENOBUFS, 0);  // no MORE: the Ring re-arms
          remove = true;
        } else {
          const io_uring_buf_ring::Ent& e = br.ents[br.head & br.mask];
          const ssize_t r = ::recv(fd, e.addr, e.len, 0);
          if (r > 0) {
            br.head++;  // consumed strictly in ring order
            push_cqe(st, w.user_data, static_cast<int>(r),
                     IORING_CQE_F_MORE | IORING_CQE_F_BUFFER |
                         (static_cast<uint32_t>(e.bid) << IORING_CQE_BUFFER_SHIFT));
          } else if (r == 0) {
            push_cqe(st, w.user_data, 0, 0);  // EOF ends the multishot, no buffer used
            remove = true;
          } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            push_cqe(st, w.user_data, -errno, 0);
            remove = true;
          }
        }
      } else if (w.opcode == IORING_OP_POLL_ADD && FD_ISSET(fd, &rset)) {
        push_cqe(st, w.user_data, POLLIN, 0);
        remove = true;
      }
    } else {
      push_cqe(st, w.user_data, -EBADF, 0);
      remove = true;
    }
    if (remove) {
      st->waiting[i] = st->waiting.back();
      st->waiting.pop_back();
    } else {
      i++;
    }
  }
}

// One select(2) pass over everything armed, no deadline. Returns false
// when there is nothing armed at all - the caller must not spin on that.
inline bool wait_ready(struct io_uring* st) {
  if (st->waiting.empty()) return false;
  fd_set rset, wset;
  const int nfds = build_waitsets(st, &rset, &wset);
  if (nfds == 0) return false;
  const int rc = ::select(nfds, &rset, &wset, nullptr, nullptr);
  if (rc <= 0) return true;  // EINTR and friends: try again from the top
  drain_ready(st, rset, wset);
  return true;
}

// The absolute deadline for a __kernel_timespec relative wait, on
// CLOCK_MONOTONIC so a wall-clock step never shortens or lengthens it.
inline struct timespec deadline_from(const struct __kernel_timespec* ts) {
  struct timespec d;
  ::clock_gettime(CLOCK_MONOTONIC, &d);
  d.tv_sec += static_cast<time_t>(ts->tv_sec);
  d.tv_nsec += static_cast<long>(ts->tv_nsec);
  if (d.tv_nsec >= 1000000000L) {
    d.tv_nsec -= 1000000000L;
    d.tv_sec += 1;
  }
  return d;
}

// One select(2) pass bounded by an absolute deadline. Returns false once
// the deadline has passed - the caller (submit_and_wait_timeout) is the
// one that turns that into -ETIME, since only it knows whether wait_nr
// was actually reached. select(0, ...) with nothing armed is a legal,
// portable sleep for the remainder, which is what makes an empty queue
// with a short timeout still block for roughly that long instead of
// returning -ETIME immediately.
inline bool wait_ready_until(struct io_uring* st, const struct timespec& deadline) {
  struct timespec now;
  ::clock_gettime(CLOCK_MONOTONIC, &now);
  int64_t remain_ns = (static_cast<int64_t>(deadline.tv_sec - now.tv_sec) * 1000000000LL) +
                      (deadline.tv_nsec - now.tv_nsec);
  if (remain_ns <= 0) return false;

  fd_set rset, wset;
  const int nfds = build_waitsets(st, &rset, &wset);
  struct timeval tv;
  tv.tv_sec = static_cast<time_t>(remain_ns / 1000000000LL);
  tv.tv_usec = static_cast<suseconds_t>((remain_ns % 1000000000LL) / 1000);
  const int rc = ::select(nfds, &rset, &wset, nullptr, &tv);
  if (rc > 0) drain_ready(st, rset, wset);
  return true;  // rc == 0 (this pass's time ran out) or rc < 0 (EINTR
                // and friends) both just loop back to the deadline
                // check above.
}

}  // namespace slipstream::detail

inline int io_uring_submit(struct io_uring* st) {
  // Immediate ops execute now, in order, with link semantics: a failing
  // IOSQE_IO_LINK member cancels the rest of its chain (-ECANCELED),
  // which the Ring's setup chains and its shutdown+close pair rely on.
  // Readiness ops park in `waiting`.
  namespace d = slipstream::detail;
  bool chain_failed = false;
  const int n = static_cast<int>(st->sq.size());
  for (struct io_uring_sqe& s : st->sq) {
    const bool linked = (s.flags & IOSQE_IO_LINK) != 0;
    if (chain_failed) {
      d::push_cqe(st, s.user_data, -ECANCELED, 0);
      if (!linked) chain_failed = false;
      continue;
    }
    if (d::deferred(s.opcode)) {
      st->waiting.push_back(s);
      continue;
    }
    const int res = d::execute(st, s);
    d::push_cqe(st, s.user_data, res, 0);
    if (linked && res < 0) chain_failed = true;
  }
  st->sq.clear();
  return n;
}

inline int io_uring_submit_and_wait(struct io_uring* st, unsigned wait_nr) {
  const int n = io_uring_submit(st);
  while (st->cq.size() < wait_nr) {
    if (!slipstream::detail::wait_ready(st)) break;  // nothing armed: do not spin
  }
  return n;
}

// Wait with a deadline: the point of this one over submit_and_wait is
// that the caller can cap how long a tick may block instead of asking
// select for that in its own way - under select, ts IS the timeout
// argument select already takes, so this costs nearly nothing here.
//
// ts == NULL means what it means in liburing: no deadline at all, so
// this degrades to exactly submit_and_wait (only wait_nr governs, and
// -ETIME can never happen - there is no timeout to run out).
//
// With a real ts, running out before wait_nr completions have arrived
// returns -ETIME, liburing's own documented behaviour for this function.
// sigmask is accepted for signature parity; nothing here can be
// interrupted by a blocked signal the way a real io_uring_enter can, so
// it does nothing.
inline int io_uring_submit_and_wait_timeout(struct io_uring* st, struct io_uring_cqe** cqe_ptr,
                                            unsigned wait_nr, struct __kernel_timespec* ts,
                                            sigset_t*) {
  namespace d = slipstream::detail;
  const int n = io_uring_submit(st);

  if (ts == nullptr) {
    while (st->cq.size() < wait_nr) {
      if (!d::wait_ready(st)) break;  // nothing armed: do not spin
    }
  } else {
    const struct timespec deadline = d::deadline_from(ts);
    while (st->cq.size() < wait_nr) {
      if (!d::wait_ready_until(st, deadline)) {
        if (cqe_ptr) *cqe_ptr = nullptr;
        return -ETIME;
      }
    }
  }

  if (cqe_ptr) *cqe_ptr = st->cq.empty() ? nullptr : &st->cq.front();
  return n;
}

#endif
