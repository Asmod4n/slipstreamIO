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
//     here a connection IS a process fd
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

// The one thing a consumer may branch on: whether it got this
// implementation. Not to pick it - that is already decided by the time
// this file is included - but because a few limits differ in kind
// here, and a caller sizing itself has to know (a connection is a
// process fd, so its count lives under FD_SETSIZE).
#define SLIPSTREAM_IO 1

#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

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
inline void io_uring_cqe_seen(struct io_uring* st, struct io_uring_cqe*) { st->cq.pop_front(); }

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
      if (fd >= FD_SETSIZE) {  // the assertion that can structurally never fire
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

// One select(2) pass over everything armed. Returns false when there is
// nothing armed at all - the caller must not spin on that.
inline bool wait_ready(struct io_uring* st) {
  if (st->waiting.empty()) return false;
  fd_set rset, wset;
  FD_ZERO(&rset);
  FD_ZERO(&wset);
  int nfds = 0;
  for (const struct io_uring_sqe& w : st->waiting) {
    const int fd = resolve_fd(st, w);
    if (fd < 0 || fd >= FD_SETSIZE) continue;
    if (w.opcode == IORING_OP_SEND || w.opcode == IORING_OP_SENDMSG) FD_SET(fd, &wset);
    else FD_SET(fd, &rset);
    if (fd + 1 > nfds) nfds = fd + 1;
  }
  if (nfds == 0) return false;
  const int rc = ::select(nfds, &rset, &wset, nullptr, nullptr);
  if (rc <= 0) return true;  // EINTR and friends: try again from the top

  for (size_t i = 0; i < st->waiting.size();) {
    struct io_uring_sqe& w = st->waiting[i];
    const int fd = resolve_fd(st, w);
    bool remove = false;
    if (fd >= 0 && fd < FD_SETSIZE) {
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
          if (nf >= FD_SETSIZE || st->free_slots.empty()) {
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
  return true;
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

#endif
