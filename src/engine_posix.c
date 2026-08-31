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
  IORING_OP_OPENAT2,
  255,
};

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

static int ready_now(int fd, short events) {
  struct pollfd p = { .fd = fd, .events = events };
  return poll(&p, 1, 0) > 0;
}

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

static enum verdict run_one(struct eng_op *op, int *res_out) {
  const struct io_uring_sqe *s = &op->sqe;
  void *buf = (void *) (uintptr_t) s->addr;
  ssize_t n;
  switch (s->opcode) {
    case IORING_OP_NOP:
      *res_out = 0;
      return RAN;
    case IORING_OP_CLOSE:
      n = close(s->fd);
      *res_out = n < 0 ? -errno : 0;
      return RAN;
    case IORING_OP_RECV:
      if (s->ioprio & IORING_RECV_MULTISHOT) {
        *res_out = -EOPNOTSUPP;
        return RAN;
      }
      n = recv(s->fd, buf, s->len, (int) s->msg_flags | MSG_DONTWAIT);
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
          !(s->msg_flags & MSG_DONTWAIT)) {
        op->wait_events = POLLIN;
        return PARK;
      }
      *res_out = n < 0 ? -errno : (int) n;
      return RAN;
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
      if (pollable == 0) return FILE_OP;
      if (pollable > 0 && !ready_now(s->fd, POLLIN)) {
        op->wait_events = POLLIN;
        return PARK;
      }
      n = off_is_current(s->off) ? read(s->fd, buf, s->len)
                                 : pread(s->fd, buf, s->len, (off_t) s->off);
      *res_out = n < 0 ? -errno : (int) n;
      return RAN;
    }
    case IORING_OP_WRITE: {
      const int pollable = fd_is_pollable(s->fd);
      if (pollable == 0) return FILE_OP;
      if (pollable > 0 && !ready_now(s->fd, POLLOUT)) {
        op->wait_events = POLLOUT;
        return PARK;
      }
      n = off_is_current(s->off) ? write(s->fd, buf, s->len)
                                 : pwrite(s->fd, buf, s->len, (off_t) s->off);
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
      *res_out = n < 0 ? -errno : (int) n;
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
      if (s->ioprio & IORING_ACCEPT_MULTISHOT) {
        *res_out = -EOPNOTSUPP; /* multishot is the next stretch, said plainly */
        return RAN;
      }
      if (!ready_now(s->fd, POLLIN)) {
        op->wait_events = POLLIN;
        return PARK;
      }
      struct sockaddr *sa = (struct sockaddr *) buf;
      socklen_t *sl = (socklen_t *) (uintptr_t) s->off;
#ifdef __APPLE__
      n = accept(s->fd, sa, sl);
      if (n >= 0 && (s->accept_flags & SOCK_CLOEXEC)) fcntl((int) n, F_SETFD, FD_CLOEXEC);
      if (n >= 0 && (s->accept_flags & SOCK_NONBLOCK))
        fcntl((int) n, F_SETFL, fcntl((int) n, F_GETFL) | O_NONBLOCK);
#else
      n = accept4(s->fd, sa, sl, (int) s->accept_flags);
#endif
      *res_out = n < 0 ? -errno : (int) n;
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
    if (stalls) slip_posix_poke(r); /* the queue is waiting on this */

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
  switch (run_one(op, res)) {
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
    enum verdict v = run_one(op, &res);
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
