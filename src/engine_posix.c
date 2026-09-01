/* The one implementation behind every readiness backend. poll, epoll,
 * kqueue and dispatch differ only in how they learn that a parked
 * descriptor came ready; how an op is TRIED, parked, retried, or sent
 * to the worker - and how the engine is poked - is the same machine,
 * and it lives here once.
 *
 * pread/pwrite and accept4 are names a bare -std=c11 hides; a .c of our
 * own may say what it needs on its first line, and glibc keeps accept4
 * behind _GNU_SOURCE where the BSDs show it by default. */
#ifndef _WIN32
#define _GNU_SOURCE 1
#define _DEFAULT_SOURCE 1

#include "engine_internal.h"

#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h> /* preadv2/pwritev2 and their iovec */
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
/* statx and openat2 have no portable wrappers under -std=c11; the raw
 * syscall does, self-declared the way src/uring_available.h does. */
extern long syscall(long number, ...);
#endif

const unsigned char slip_posix_carried_ops[] = {
  IORING_OP_NOP,     IORING_OP_READ,    IORING_OP_WRITE,    IORING_OP_RECV,
  IORING_OP_SEND,    IORING_OP_RECVMSG, IORING_OP_SENDMSG,  IORING_OP_ACCEPT,
  IORING_OP_CONNECT, IORING_OP_SOCKET,  IORING_OP_BIND,     IORING_OP_LISTEN,
  IORING_OP_SHUTDOWN, IORING_OP_CLOSE,  IORING_OP_POLL_ADD, IORING_OP_POLL_REMOVE,
  IORING_OP_ASYNC_CANCEL, IORING_OP_STATX, IORING_OP_UNLINKAT, IORING_OP_OPENAT,
  IORING_OP_OPENAT2, IORING_OP_URING_CMD,
  255,
};

/* ---- provided buffers -------------------------------------------------
 * The ring's entries live in the CALLER's memory: it fills them and
 * advances the tail with a release store, the engine consumes at its own
 * head. Both indices are free-running 16-bit counters masked by the ring
 * size, which is how the kernel reads the same memory. */

static int bufring_take(struct slip_ring *r, unsigned short bgid, void **addr,
                        unsigned *len, unsigned short *bid) {
  struct slip_bufring *b = slip_bufring_of(r, bgid);
  if (b == NULL) return -ENOBUFS;
  const unsigned short tail = __atomic_load_n(&b->bufs[0].resv, __ATOMIC_ACQUIRE);
  if (tail == b->head) return -ENOBUFS;
  const struct io_uring_buf *e = &b->bufs[b->head & (b->entries - 1)];
  *addr = (void *) (uintptr_t) e->addr;
  *len = e->len;
  *bid = e->bid;
  b->head++;
  return 0;
}

/* A buffer taken for a recv that then had nothing to read goes back: the
 * kernel consumes one only when it hands bytes over with
 * IORING_CQE_F_BUFFER, and an entry taken without a CQE would be one the
 * caller never learns to refill. Only the engine thread consumes, so
 * rewinding the head is the whole of it. */
static void bufring_unget(struct slip_ring *r, unsigned short bgid) {
  struct slip_bufring *b = slip_bufring_of(r, bgid);
  if (b != NULL) b->head--;
}

/* ---- direct descriptors -----------------------------------------------
 * An op that INSTANTIATES a descriptor (socket, accept, open) may put it
 * straight into the fixed table instead of handing back a number:
 * file_index is the slot plus one, or IORING_FILE_INDEX_ALLOC to let the
 * table pick inside its alloc range. The completion then carries the
 * chosen slot for ALLOC and 0 for a named one - the kernel's own two
 * answers. */
static int install_direct(struct slip_ring *r, const struct io_uring_sqe *s, int newfd) {
  const int slot = slip_fixed_install(r, s->file_index, newfd);
  if (slot < 0) {
    close(newfd);
    return slot;
  }
  return s->file_index == IORING_FILE_INDEX_ALLOC ? slot : 0;
}

/* ---- the poke pipe ---------------------------------------------------- */

int slip_posix_ctl_open(struct slip_ring *r) {
  int fds[2];
  if (pipe(fds) != 0) return -1;
  r->ctl_r = fds[0];
  r->ctl_w = fds[1];
  /* Drained until empty on every wakeup; it must never block. */
  fcntl(r->ctl_r, F_SETFL, O_NONBLOCK);
  return 0;
}

void slip_posix_ctl_close(struct slip_ring *r) {
  if (r->ctl_r >= 0) close(r->ctl_r);
  if (r->ctl_w >= 0) close(r->ctl_w);
  r->ctl_r = r->ctl_w = -1;
}

void slip_posix_ctl_drain(struct slip_ring *r) {
  char sink[64];
  while (read(r->ctl_r, sink, sizeof(sink)) > 0) { }
}

/* The fixed file table's close, in this family's spelling - the core
 * holds the table but no OS call. */
void slip_native_fd_close(int fd) { close(fd); }

void slip_posix_poke(struct slip_ring *r) {
  const char b = 1;
  /* A full pipe already holds a wakeup; a failed write is not an error. */
  (void) !write(r->ctl_w, &b, 1);
}

/* ---- running one op ---------------------------------------------------
 * Three answers: done, would-block (park it with these events), or file
 * (a regular file has no readiness to poll for - the worker runs it). */

enum verdict { RAN, PARK, FILE_OP };

/* liburing spells "wherever the descriptor stands" as an offset of -1,
 * and that is the only spelling answered with read/write rather than
 * pread/pwrite - a socket or a pipe has no offset. */
static int off_is_current(__u64 off) { return off == (__u64) -1; }

/* 1 readiness, 0 regular file, -1 the descriptor itself is broken - and
 * then the REAL call runs right away and reports it, because parking a
 * bad descriptor waits forever: poll ignores negative fds, so a parked
 * read on fd -1 would never fire, where the kernel answers -EBADF. */
static int fd_is_pollable(int fd) {
  struct stat st;
  if (fstat(fd, &st) != 0) return -1;
  return !(S_ISREG(st.st_mode) || S_ISBLK(st.st_mode) || S_ISDIR(st.st_mode));
}

/* io_uring TRIES, it never asks first: every op is issued nonblocking
 * and -EAGAIN is the answer that sends it to the poll set. A pre-flight
 * poll() would be a second syscall per op for information the op itself
 * hands back. Userspace cannot pass the kernel's internal force_nonblock
 * to accept, so the descriptors this engine touches carry O_NONBLOCK
 * instead - set once, where they are made or first used. */
static void set_nonblock(int fd) {
  const int fl = fcntl(fd, F_GETFL, 0);
  if (fl >= 0 && !(fl & O_NONBLOCK)) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

#ifdef __linux__
#ifndef RWF_NOWAIT
#define RWF_NOWAIT 0x00000008
#endif
/* The page cache question, answered the way io_uring answers it: the
 * read is ISSUED with NOWAIT, which serves whatever is resident and says
 * -EAGAIN for the rest. A short read is a normal answer here - only the
 * cached part came back - and that is what the kernel's own ring does
 * before it pays for a worker. mincore() cannot replace this: it speaks
 * about a mapping, and about the moment before the read. */
static ssize_t file_try_nowait(const struct io_uring_sqe *s, void *buf, int writing) {
  struct iovec iov = { .iov_base = buf, .iov_len = s->len };
  const off_t off = off_is_current(s->off) ? (off_t) -1 : (off_t) s->off;
  return writing ? pwritev2(s->fd, &iov, 1, off, RWF_NOWAIT)
                 : preadv2(s->fd, &iov, 1, off, RWF_NOWAIT);
}

/* Whether the miss is worth a thread, or is the real answer. */
static int nowait_missed(void) {
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EOPNOTSUPP ||
         errno == ENOSYS || errno == EINVAL;
}
#endif

#ifndef __linux__
#include <linux/stat.h> /* the shim's struct statx, for hosts without one */

/* statx filled from fstatat: the basic stats, and stx_mask honest about
 * exactly that. macOS spells the timespec fields its own way. */
static int slip_statx_from_stat(int dfd, const char *path, int flags, void *stxbuf) {
  struct stat st;
  int rc;
  const int at_flags = (flags & AT_SYMLINK_NOFOLLOW) ? AT_SYMLINK_NOFOLLOW : 0;
#ifdef AT_EMPTY_PATH
  if ((flags & AT_EMPTY_PATH) && path[0] == '\0')
    rc = fstat(dfd, &st);
  else
#endif
    rc = fstatat(dfd, path, &st, at_flags);
  if (rc != 0) return -errno;
  struct statx *x = stxbuf;
  memset(x, 0, sizeof(*x));
  x->stx_mask = STATX_BASIC_STATS;
  x->stx_blksize = (__u32) st.st_blksize;
  x->stx_nlink = (__u32) st.st_nlink;
  x->stx_uid = (__u32) st.st_uid;
  x->stx_gid = (__u32) st.st_gid;
  x->stx_mode = (__u16) st.st_mode;
  x->stx_ino = (__u64) st.st_ino;
  x->stx_size = (__u64) st.st_size;
  x->stx_blocks = (__u64) st.st_blocks;
#ifdef __APPLE__
  x->stx_atime.tv_sec = st.st_atimespec.tv_sec;
  x->stx_atime.tv_nsec = (__u32) st.st_atimespec.tv_nsec;
  x->stx_mtime.tv_sec = st.st_mtimespec.tv_sec;
  x->stx_mtime.tv_nsec = (__u32) st.st_mtimespec.tv_nsec;
  x->stx_ctime.tv_sec = st.st_ctimespec.tv_sec;
  x->stx_ctime.tv_nsec = (__u32) st.st_ctimespec.tv_nsec;
#else
  x->stx_atime.tv_sec = st.st_atim.tv_sec;
  x->stx_atime.tv_nsec = (__u32) st.st_atim.tv_nsec;
  x->stx_mtime.tv_sec = st.st_mtim.tv_sec;
  x->stx_mtime.tv_nsec = (__u32) st.st_mtim.tv_nsec;
  x->stx_ctime.tv_sec = st.st_ctim.tv_sec;
  x->stx_ctime.tv_nsec = (__u32) st.st_ctim.tv_nsec;
#endif
  return 0;
}
#endif

/* accept4 where it exists, spelled out where it does not. */
static int accept_one(const struct io_uring_sqe *s) {
  struct sockaddr *sa = (struct sockaddr *) (uintptr_t) s->addr;
  socklen_t *sl = (socklen_t *) (uintptr_t) s->off;
#ifdef __APPLE__
  const int fd = accept(s->fd, sa, sl);
  if (fd >= 0 && (s->accept_flags & SOCK_CLOEXEC)) fcntl(fd, F_SETFD, FD_CLOEXEC);
  if (fd >= 0 && (s->accept_flags & SOCK_NONBLOCK))
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  return fd;
#else
  return accept4(s->fd, sa, sl, (int) s->accept_flags);
#endif
}

/* MULTISHOT: one submission, a CQE per event, each carrying
 * IORING_CQE_F_MORE to say the op is still armed. The op itself is never
 * completed here - it goes back to the parked set and the LAST word is
 * the one that returns RAN, without F_MORE, exactly as the kernel ends a
 * multishot. */
static enum verdict run_accept_multishot(struct slip_ring *r, struct eng_op *op,
                                         int *res_out) {
  const struct io_uring_sqe *s = &op->sqe;
  for (;;) {
    set_nonblock(s->fd); /* accept takes its nonblocking from the listener */
    const int fd = accept_one(s);
    if (fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        op->wait_events = POLLIN;
        return PARK;
      }
      *res_out = -errno;
      return RAN;
    }
    const int answer = s->file_index != 0 ? install_direct(r, s, fd) : fd;
    if (answer < 0) {
      *res_out = answer;
      return RAN;
    }
    slip_engine_emit(r, s->user_data, answer, IORING_CQE_F_MORE);
  }
}

static enum verdict run_recv_multishot(struct slip_ring *r, struct eng_op *op,
                                       int *res_out) {
  const struct io_uring_sqe *s = &op->sqe;
  for (;;) {
    void *rbuf = (void *) (uintptr_t) s->addr;
    unsigned rlen = s->len;
    unsigned short bid = 0;
    unsigned cflags = 0;
    const int select = (s->flags & IOSQE_BUFFER_SELECT) != 0;
    if (select) {
      const int rc = bufring_take(r, s->buf_group, &rbuf, &rlen, &bid);
      if (rc != 0) {
        /* Out of buffers ENDS a multishot recv - the caller refills and
         * arms a new one; that is the kernel's contract, not a stall. */
        *res_out = rc;
        return RAN;
      }
      cflags = IORING_CQE_F_BUFFER | ((unsigned) bid << IORING_CQE_BUFFER_SHIFT);
    }
    const ssize_t n = recv(s->fd, rbuf, rlen, (int) s->msg_flags | MSG_DONTWAIT);
    if (n > 0) {
      slip_engine_emit(r, s->user_data, (int) n, cflags | IORING_CQE_F_MORE);
      continue;
    }
    /* Nothing came: the buffer was never filled, so it is not consumed. */
    if (select) bufring_unget(r, s->buf_group);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      op->wait_events = POLLIN;
      return PARK;
    }
    *res_out = n < 0 ? -errno : 0; /* 0 is EOF, and it ends the multishot */
    return RAN;
  }
}

/* SOCKET_URING_OP_*: the socket calls, as ring ops - plain syscalls, so
 * both families answer them from here.
 *
 * EVERY FIELD BELOW IS READ WHERE LIBURING WRITES IT, and liburing is
 * MIT: io_uring_prep_cmd_sock fills level, optname, optval and optlen;
 * io_uring_prep_cmd_getsockname fills addr with the sockaddr, addr3
 * with its socklen_t*, and optlen with 0 for this socket or 1 for the
 * peer. The unions overlap - addr carries level and optname for the
 * sockopt commands, addr3 and optval are the same word - so each
 * command reads the member ITS OWN prep function wrote.
 *
 * What the header and those functions leave open was settled by asking
 * a running kernel and comparing answers (test/parity.c), never by
 * reading kernel sources. */
int slip_posix_cmd_sock(const struct io_uring_sqe *s) {
  ssize_t n;
  switch (s->cmd_op) {
    case SOCKET_URING_OP_SETSOCKOPT:
      n = setsockopt(s->fd, (int) s->level, (int) s->optname,
                     (const void *) (uintptr_t) s->optval, (socklen_t) s->optlen);
      return n < 0 ? -errno : 0;
    case SOCKET_URING_OP_GETSOCKOPT: {
      socklen_t len = (socklen_t) s->optlen;
      n = getsockopt(s->fd, (int) s->level, (int) s->optname,
                     (void *) (uintptr_t) s->optval, &len);
      /* The kernel answers the LENGTH it wrote, not zero. */
      return n < 0 ? -errno : (int) len;
    }
    case SOCKET_URING_OP_GETSOCKNAME: {
      struct sockaddr *sa = (struct sockaddr *) (uintptr_t) s->addr;
      socklen_t *len = (socklen_t *) (uintptr_t) s->addr3;
      if (sa == NULL || len == NULL) return -EFAULT;
      n = s->optlen != 0 ? getpeername(s->fd, sa, len) : getsockname(s->fd, sa, len);
      return n < 0 ? -errno : 0;
    }
    default:
      return -EOPNOTSUPP;
  }
}

static enum verdict run_one(struct slip_ring *r, struct eng_op *op, int *res_out) {
  const struct io_uring_sqe *s = &op->sqe;
  void *buf = (void *) (uintptr_t) s->addr;
  ssize_t n;
  switch (s->opcode) {
    case IORING_OP_NOP:
      *res_out = 0;
      return RAN;
    case IORING_OP_CLOSE:
      /* close_direct names a SLOT in file_index, not a descriptor: the
       * table hands the real one over and forgets it, and this closes
       * it. A plain close carries no file_index and closes its fd. */
      if (s->file_index != 0) {
        const unsigned slot = s->file_index - 1;
        const int real = slip_fixed_lookup(r, (int) slot);
        if (real < 0) {
          *res_out = real;
          return RAN;
        }
        slip_fixed_clear(r, slot);
        *res_out = close(real) < 0 ? -errno : 0;
        return RAN;
      }
      n = close(s->fd);
      *res_out = n < 0 ? -errno : 0;
      return RAN;
    case IORING_OP_RECV: {
      /* A bundle answers ONE completion for several buffers at once, and
       * this engine never reports IORING_FEAT_RECVSEND_BUNDLE - a caller
       * that asks anyway is told, not quietly served one buffer. */
      if (s->ioprio & IORING_RECVSEND_BUNDLE) {
        *res_out = -EOPNOTSUPP;
        return RAN;
      }
      if (s->ioprio & IORING_RECV_MULTISHOT) return run_recv_multishot(r, op, res_out);
      void *rbuf = buf;
      unsigned rlen = s->len;
      unsigned short bid = 0;
      unsigned cflags = 0;
      const int select = (s->flags & IOSQE_BUFFER_SELECT) != 0;
      if (select) {
        const int rc = bufring_take(r, s->buf_group, &rbuf, &rlen, &bid);
        if (rc != 0) {
          *res_out = rc;
          return RAN;
        }
        cflags = IORING_CQE_F_BUFFER | ((unsigned) bid << IORING_CQE_BUFFER_SHIFT);
      }
      n = recv(s->fd, rbuf, rlen, (int) s->msg_flags | MSG_DONTWAIT);
      if (n <= 0 && select) bufring_unget(r, s->buf_group); /* never filled, never consumed */
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
          !(s->msg_flags & MSG_DONTWAIT)) {
        op->wait_events = POLLIN;
        return PARK;
      }
      if (n > 0) op->cqe_flags = cflags;
      *res_out = n < 0 ? -errno : (int) n;
      return RAN;
    }
    case IORING_OP_SEND:
      n = send(s->fd, buf, s->len, (int) s->msg_flags | MSG_DONTWAIT);
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
          !(s->msg_flags & MSG_DONTWAIT)) {
        op->wait_events = POLLOUT;
        return PARK;
      }
      *res_out = n < 0 ? -errno : (int) n;
      return RAN;
    case IORING_OP_READ: {
      const int pollable = fd_is_pollable(s->fd);
      if (pollable == 0) {
#ifdef __linux__
        n = file_try_nowait(s, buf, 0);
        if (n >= 0) {
          *res_out = (int) n; /* served out of the page cache, on this thread */
          return RAN;
        }
        if (!nowait_missed()) {
          *res_out = -errno;
          return RAN;
        }
#endif
        return FILE_OP; /* a real disk wait: that is what the worker is for */
      }
      if (pollable > 0) set_nonblock(s->fd);
      n = off_is_current(s->off) ? read(s->fd, buf, s->len)
                                 : pread(s->fd, buf, s->len, (off_t) s->off);
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        op->wait_events = POLLIN;
        return PARK;
      }
      *res_out = n < 0 ? -errno : (int) n;
      return RAN;
    }
    case IORING_OP_WRITE: {
      const int pollable = fd_is_pollable(s->fd);
      if (pollable == 0) {
#ifdef __linux__
        n = file_try_nowait(s, buf, 1);
        if (n >= 0) {
          *res_out = (int) n;
          return RAN;
        }
        if (!nowait_missed()) {
          *res_out = -errno;
          return RAN;
        }
#endif
        return FILE_OP;
      }
      if (pollable > 0) set_nonblock(s->fd);
      n = off_is_current(s->off) ? write(s->fd, buf, s->len)
                                 : pwrite(s->fd, buf, s->len, (off_t) s->off);
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        op->wait_events = POLLOUT;
        return PARK;
      }
      *res_out = n < 0 ? -errno : (int) n;
      return RAN;
    }
    case IORING_OP_SOCKET:
      /* fd carries the domain, off the type, len the protocol -
       * io_uring_prep_socket. The flags word is refused like the kernel
       * refuses what it does not know. */
      if (s->rw_flags != 0) {
        *res_out = -EINVAL;
        return RAN;
      }
      n = socket(s->fd, (int) s->off, (int) s->len);
      if (n < 0) {
        *res_out = -errno;
        return RAN;
      }
      *res_out = s->file_index != 0 ? install_direct(r, s, (int) n) : (int) n;
      return RAN;
    case IORING_OP_BIND:
      n = bind(s->fd, (const struct sockaddr *) buf, (socklen_t) s->off);
      *res_out = n < 0 ? -errno : 0;
      return RAN;
    case IORING_OP_LISTEN:
      n = listen(s->fd, (int) s->len);
      *res_out = n < 0 ? -errno : 0;
      return RAN;
    case IORING_OP_SHUTDOWN:
      n = shutdown(s->fd, (int) s->len);
      *res_out = n < 0 ? -errno : 0;
      return RAN;
    case IORING_OP_ACCEPT: {
      if (s->ioprio & IORING_ACCEPT_MULTISHOT) return run_accept_multishot(r, op, res_out);
      set_nonblock(s->fd);
      n = accept_one(s);
      if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          op->wait_events = POLLIN;
          return PARK;
        }
        *res_out = -errno;
        return RAN;
      }
      *res_out = s->file_index != 0 ? install_direct(r, s, (int) n) : (int) n;
      return RAN;
    }
    case IORING_OP_RECVMSG:
      if (s->ioprio & IORING_RECV_MULTISHOT) {
        *res_out = -EOPNOTSUPP;
        return RAN;
      }
      n = recvmsg(s->fd, (struct msghdr *) buf, (int) s->msg_flags | MSG_DONTWAIT);
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
          !(s->msg_flags & MSG_DONTWAIT)) {
        op->wait_events = POLLIN;
        return PARK;
      }
      *res_out = n < 0 ? -errno : (int) n;
      return RAN;
    case IORING_OP_SENDMSG:
      n = sendmsg(s->fd, (const struct msghdr *) buf, (int) s->msg_flags | MSG_DONTWAIT);
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
          !(s->msg_flags & MSG_DONTWAIT)) {
        op->wait_events = POLLOUT;
        return PARK;
      }
      *res_out = n < 0 ? -errno : (int) n;
      return RAN;
    case IORING_OP_POLL_ADD: {
      if (s->len & IORING_POLL_ADD_MULTI) {
        *res_out = -EOPNOTSUPP;
        return RAN;
      }
      /* The readiness IS the answer: ready now completes with revents,
       * not ready parks on the asked directions - the backends deliver
       * ERR/HUP regardless, the way poll itself does. poll32_events is
       * host order on little endian (liburing swaps only on BE). */
      struct pollfd p = { .fd = s->fd, .events = (short) s->poll32_events };
      n = poll(&p, 1, 0);
      if (n < 0) {
        *res_out = -errno;
        return RAN;
      }
      if (n == 0) {
        op->wait_events = (short) (s->poll32_events & (POLLIN | POLLOUT));
        return PARK;
      }
      *res_out = (int) (unsigned short) p.revents;
      return RAN;
    }
    case IORING_OP_STATX: {
      const char *path = (const char *) buf;
      void *stx = (void *) (uintptr_t) s->off;
#ifdef __linux__
      n = syscall(SYS_statx, s->fd, path, (int) s->statx_flags, (unsigned) s->len, stx);
      *res_out = n < 0 ? -errno : 0;
#else
      *res_out = slip_statx_from_stat(s->fd, path, (int) s->statx_flags, stx);
#endif
      return RAN;
    }
    case IORING_OP_UNLINKAT:
      n = unlinkat(s->fd, (const char *) buf, (int) s->unlink_flags);
      *res_out = n < 0 ? -errno : 0;
      return RAN;
    case IORING_OP_URING_CMD:
      *res_out = slip_posix_cmd_sock(s);
      return RAN;
    case IORING_OP_CONNECT:
    case IORING_OP_OPENAT:
    case IORING_OP_OPENAT2:
      /* connect blocks until the peer answers, open blocks on a FIFO -
       * the kernel runs both async and so does the worker. */
      return FILE_OP;
    default:
      /* Known op, not carried here. -EOPNOTSUPP and not -EINVAL: the
       * first says "that op, not here", which is what a caller needs in
       * order to take another route. */
      *res_out = -EOPNOTSUPP;
      return RAN;
  }
}

/* ---- the worker: regular files, run to the end ------------------------
 * Blocking is the point: this thread exists so that the engine's wait
 * loop never does. */

static int run_file_op(const struct io_uring_sqe *s) {
  void *buf = (void *) (uintptr_t) s->addr;
  ssize_t n;
  switch (s->opcode) {
    case IORING_OP_READ:
      n = off_is_current(s->off) ? read(s->fd, buf, s->len)
                                 : pread(s->fd, buf, s->len, (off_t) s->off);
      break;
    case IORING_OP_WRITE:
      n = off_is_current(s->off) ? write(s->fd, buf, s->len)
                                 : pwrite(s->fd, buf, s->len, (off_t) s->off);
      break;
    case IORING_OP_RECV:
      n = recv(s->fd, buf, s->len, (int) s->msg_flags);
      break;
    case IORING_OP_SEND:
      n = send(s->fd, buf, s->len, (int) s->msg_flags);
      break;
    case IORING_OP_CONNECT:
      n = connect(s->fd, (const struct sockaddr *) buf, (socklen_t) s->off);
      if (n == 0) return 0;
      break;
    case IORING_OP_OPENAT:
      n = openat(s->fd, (const char *) buf, (int) s->open_flags, (mode_t) s->len);
      break;
    case IORING_OP_OPENAT2: {
      /* addr2 carries a struct open_how of len bytes - the shape shared
       * with the kernel. Off Linux only resolve-free asks translate to
       * openat; a resolve constraint cannot be kept and is refused. */
      const struct slip_open_how {
        unsigned long long flags, mode, resolve;
      } *how = (const void *) (uintptr_t) s->off;
      if (s->len < sizeof(*how)) return -EINVAL;
#ifdef __linux__
      n = syscall(SYS_openat2, s->fd, (const char *) buf, how, (size_t) s->len);
#else
      if (how->resolve != 0) return -EOPNOTSUPP;
      n = openat(s->fd, (const char *) buf, (int) how->flags, (mode_t) how->mode);
#endif
      break;
    }
    default:
      return -EOPNOTSUPP;
  }
  return n < 0 ? -errno : (int) n;
}

static int worker_main(void *arg) {
  struct slip_ring *r = arg;
  mtx_lock(&r->mtx);
  for (;;) {
    while (r->wq_head == NULL && !r->stopping) cnd_wait(&r->wq_cv, &r->mtx);
    if (r->stopping) break;
    struct eng_op *op = r->wq_head;
    r->wq_head = op->next;
    if (r->wq_head == NULL) r->wq_tail = NULL;
    mtx_unlock(&r->mtx);

    const int res = run_file_op(&op->sqe);
    const int stalls = op->stalls_queue;
    if (stalls) {
      /* The ticket goes up before post frees the op. The engine only
       * compares the pointer against r->blocking, never reads through
       * it. */
      mtx_lock(&r->mtx);
      r->blocked_done = op;
      r->blocked_failed = res < 0;
      mtx_unlock(&r->mtx);
    }
    slip_engine_post(r, op, res); /* frees op */
    /* ALWAYS, not just for a stalled chain: the submitter waits inside
     * the backend now, and this pipe is the only door into that wait.
     * While an engine thread owned the loop, posting alone was enough -
     * the waiter sat on a condvar that posting signalled. */
    slip_posix_poke(r);

    mtx_lock(&r->mtx);
  }
  mtx_unlock(&r->mtx);
  return 0;
}

static void worker_start_once(struct slip_ring *r) {
  if (r->worker_live) return;
  if (thrd_create(&r->worker, worker_main, r) == thrd_success) r->worker_live = 1;
}

/* ---- parking ---------------------------------------------------------- */

/* 1 if the op now waits with the backend armed; 0 if the set is full or
 * the backend refused. */
static int park(struct slip_ring *r, struct eng_op *op) {
  if (r->waiting_n >= SLIP_WAITING_MAX) return 0;
  if (r->be->arm(r, op) != 0) return 0;
  r->waiting[r->waiting_n++] = op;
  return 1;
}

/* Removal first, then disarm: a backend that merges per-descriptor
 * interest recomputes it from waiting[], which must no longer hold the
 * leaving op. */
static void unpark(struct slip_ring *r, struct eng_op *op) {
  for (unsigned i = 0; i < r->waiting_n; i++) {
    if (r->waiting[i] == op) {
      r->waiting[i] = r->waiting[--r->waiting_n];
      r->be->disarm(r, op);
      return;
    }
  }
}

void slip_posix_hand_to_worker(struct slip_ring *r, struct eng_op *op) {
  /* stalls_queue rides on the op BEFORE the worker can see it - the
   * ticket compare in the core needs it set by then. */
  if (op->sqe.flags & IOSQE_IO_LINK) op->stalls_queue = 1;
  mtx_lock(&r->mtx);
  if (r->wq_tail) r->wq_tail->next = op;
  else r->wq_head = op;
  r->wq_tail = op;
  worker_start_once(r);
  cnd_signal(&r->wq_cv);
  mtx_unlock(&r->mtx);
}

/* 1 when the cancel op names this target. POLL_REMOVE only ever aims
 * at poll_add; ASYNC_CANCEL aims by user_data, or by descriptor with
 * IORING_ASYNC_CANCEL_FD. */
static int cancel_names(const struct eng_op *cancel, const struct eng_op *target) {
  if (cancel->sqe.opcode == IORING_OP_POLL_REMOVE)
    return target->sqe.opcode == IORING_OP_POLL_ADD &&
           target->sqe.user_data == cancel->sqe.addr;
  if (cancel->sqe.cancel_flags & IORING_ASYNC_CANCEL_FD)
    return target->sqe.fd == cancel->sqe.fd;
  return target->sqe.user_data == cancel->sqe.addr;
}

static void cancel_target(struct slip_ring *r, struct eng_op *t) {
  if (t == r->blocking) {
    r->blocking = NULL;
    r->chain_failed = 1;
  } else if (t->sqe.flags & IOSQE_IO_LINK) {
    r->chain_failed = 1;
  }
  slip_engine_post(r, t, -ECANCELED);
}

/* The kernel's answers, kept: 0 for the one cancelled (the count with
 * CANCEL_ALL), -ENOENT for no match, -EALREADY for an op the worker is
 * already inside - past recall, like the kernel's running ops. */
static int cancel_matching(struct slip_ring *r, struct eng_op *op) {
  const int all = (op->sqe.opcode == IORING_OP_ASYNC_CANCEL) &&
                  (op->sqe.cancel_flags & IORING_ASYNC_CANCEL_ALL);
  int count = 0;

  for (unsigned i = 0; i < r->waiting_n;) {
    if (cancel_names(op, r->waiting[i])) {
      struct eng_op *t = r->waiting[i];
      unpark(r, t); /* swap-removes index i; rescan the same slot */
      cancel_target(r, t);
      count++;
      if (!all) return 0;
    } else {
      i++;
    }
  }

  for (struct eng_op **p = &r->queue_head; *p != NULL;) {
    if (cancel_names(op, *p)) {
      struct eng_op *t = *p;
      *p = t->next;
      if (r->queue_tail == t) {
        r->queue_tail = NULL;
        for (struct eng_op *q = r->queue_head; q != NULL; q = q->next) r->queue_tail = q;
      }
      cancel_target(r, t);
      count++;
      if (!all) return 0;
    } else {
      p = &(*p)->next;
    }
  }

  mtx_lock(&r->mtx);
  for (struct eng_op **p = &r->wq_head; *p != NULL;) {
    if (cancel_names(op, *p)) {
      struct eng_op *t = *p;
      *p = t->next;
      if (r->wq_tail == t) {
        r->wq_tail = NULL;
        for (struct eng_op *q = r->wq_head; q != NULL; q = q->next) r->wq_tail = q;
      }
      mtx_unlock(&r->mtx);
      cancel_target(r, t);
      count++;
      if (!all) return 0;
      mtx_lock(&r->mtx);
    } else {
      p = &(*p)->next;
    }
  }
  mtx_unlock(&r->mtx);

  if (count > 0) return all ? count : 0;
  /* The one place left is inside the worker right now. */
  if (r->blocking != NULL && r->blocked_done == NULL) return -EALREADY;
  return -ENOENT;
}

int slip_posix_execute(struct slip_ring *r, struct eng_op *op, int *res) {
  if (op->sqe.opcode == IORING_OP_ASYNC_CANCEL || op->sqe.opcode == IORING_OP_POLL_REMOVE) {
    *res = cancel_matching(r, op);
    return EXEC_DONE;
  }
  switch (run_one(r, op, res)) {
    case RAN:
      return EXEC_DONE;
    case PARK:
      if (park(r, op)) return EXEC_PENDING;
      *res = -EBUSY; /* a full waiting set is refused, not dropped */
      return EXEC_DONE;
    case FILE_OP:
      slip_posix_hand_to_worker(r, op);
      return EXEC_PENDING;
  }
  *res = -EINVAL;
  return EXEC_DONE;
}

int slip_posix_finish_ready(struct slip_ring *r, struct eng_op **ready, unsigned ready_n,
                            struct eng_done *out, unsigned max) {
  unsigned n = 0;
  for (unsigned i = 0; i < ready_n && n < max; i++) {
    struct eng_op *op = ready[i];
    unpark(r, op);
    int res = 0;
    enum verdict v = run_one(r, op, &res);
    if (v == FILE_OP) { /* a parked op never becomes a file op */
      v = RAN;
      res = -EOPNOTSUPP;
    }
    if (v == RAN) {
      out[n].op = op;
      out[n].res = res;
      n++;
    } else if (!park(r, op)) { /* spurious wakeup, and now the set is full */
      out[n].op = op;
      out[n].res = -EBUSY;
      n++;
    }
  }
  return (int) n;
}

#else
typedef int slip_engine_posix_is_not_windows;
#endif
