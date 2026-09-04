/* The Windows backend, and the only completion-shaped one: IOCP is the
 * one foreign API that matches io_uring's model instead of being
 * interpreted on top of it. Nothing parks - a recv IS issued, the OS
 * holds it, and the port hands back a packet when it is done. execute
 * therefore answers EXEC_PENDING with the op already in flight, wait is
 * GetQueuedCompletionStatus, and the poke is a posted packet - no pipe
 * anywhere.
 *
 * Carried today: NOP, RECV, SEND, CLOSE, and the socket lifecycle -
 * SOCKET, BIND, LISTEN, SHUTDOWN, which are plain Winsock calls that
 * answer at once; ACCEPT and CONNECT through AcceptEx and ConnectEx;
 * POLL_ADD for POLLIN; and file IO - OPENAT, READ, WRITE - which works
 * only because the OPEN is the engine's own: a CRT descriptor's handle
 * is not FILE_FLAG_OVERLAPPED, and without that flag ReadFile carries
 * no offset and completes through no port.
 * The SQE's fd field is 32 bits by liburing's own ABI;
 * Windows SOCKET values fit it in practice and Wine's always do.
 *
 * Ops in flight ride a wrapper that embeds the OVERLAPPED, so a packet
 * maps back with CONTAINING_RECORD. close with ops still pending
 * cancels them and drains the port until every wrapper came home -
 * the kernel writes into OVERLAPPED memory, so freeing early is the
 * use-after-free, not a leak. */
#ifdef _WIN32

#include "engine_internal.h"

#include <winsock2.h>
#include <ws2tcpip.h>
/* AcceptEx, ConnectEx and their GUIDs live here, not in winsock2.h. */
#include <mswsock.h>
#include <windows.h>
#include <fcntl.h>
/* Windows has no openat, so it has no AT_FDCWD either. The value is
 * Linux's, and it is the only dfd this backend accepts - see OPENAT. */
#ifndef AT_FDCWD
#define AT_FDCWD (-100)
#endif
#include <io.h> /* _close, _open_osfhandle: a CRT descriptor is not a SOCKET */
#include <stdlib.h>

/* What a packet means when it comes home. A recv's answer is its byte
 * count and needs nothing more; an accept's is a SOCKET, and Winsock
 * asks for one more call before that socket behaves like any other. */
enum iocp_kind { IOCP_KIND_IO, IOCP_KIND_ACCEPT, IOCP_KIND_CONNECT, IOCP_KIND_POLL };

struct iocp_op {
  OVERLAPPED ov;
  struct eng_op *op;
  struct iocp_op *next; /* the in-flight list, engine thread only */
  SOCKET sock;
  WSABUF wb;
  unsigned char kind;
  SOCKET accepted; /* ACCEPT: the socket AcceptEx was given */
  /* AcceptEx writes both addresses here and wants 16 bytes of slack
   * after each - Microsoft's documented requirement, not a guess. */
  char addrs[2 * (sizeof(struct sockaddr_storage) + 16)];
};

struct iocp_state {
  HANDLE port;
  struct iocp_op *in_flight;
};

static const ULONG_PTR IOCP_KEY_POKE = 1;
static const ULONG_PTR IOCP_KEY_IO = 2;

static int win_err_to_errno(DWORD e) {
  switch (e) {
    case ERROR_HANDLE_EOF: return 0;
    case WSAECONNRESET:
    case ERROR_NETNAME_DELETED: return -ECONNRESET;
    case WSAECONNABORTED: return -ECONNABORTED;
    case WSAENOTSOCK: return -ENOTSOCK;
    case WSAESHUTDOWN: return -EPIPE;
    case ERROR_OPERATION_ABORTED: return -ECANCELED;
    default: return -EIO;
  }
}

static int iocp_open_ring(struct slip_ring *r) {
  /* The engine never speaks to the network itself, but WSARecv on a
   * socket the CALLER made still needs Winsock up in this process;
   * asking twice is defined and refcounted. */
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
  struct iocp_state *st = calloc(1, sizeof(*st));
  if (st == NULL) {
    WSACleanup();
    return -1;
  }
  st->port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
  if (st->port == NULL) {
    free(st);
    WSACleanup();
    return -1;
  }
  r->be_state = st;
  return 0;
}

/* The fixed file table's close: a descriptor here came from socket() or
 * from the CRT, and closesocket refuses the latter - the same two-step
 * IORING_OP_CLOSE takes. */
void slip_native_fd_close(int fd) {
  if (closesocket((SOCKET) fd) != 0) (void) _close(fd);
}

static void iocp_close_ring(struct slip_ring *r) {
  struct iocp_state *st = r->be_state;
  if (st == NULL) return;
  /* Every issued OVERLAPPED must come home before its memory may go -
   * the kernel writes into it. Cancel them all, then drain the port
   * until the list is empty. */
  for (struct iocp_op *w = st->in_flight; w != NULL; w = w->next)
    CancelIoEx((HANDLE) w->sock, &w->ov);
  while (st->in_flight != NULL) {
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    OVERLAPPED *ov = NULL;
    if (!GetQueuedCompletionStatus(st->port, &bytes, &key, &ov, 1000) && ov == NULL) break;
    if (key == IOCP_KEY_IO && ov != NULL) {
      struct iocp_op *w = CONTAINING_RECORD(ov, struct iocp_op, ov);
      struct iocp_op **p = &st->in_flight;
      while (*p != NULL && *p != w) p = &(*p)->next;
      if (*p == w) *p = w->next;
      free(w->op);
      free(w);
    }
  }
  CloseHandle(st->port);
  free(st);
  r->be_state = NULL;
  WSACleanup();
}

static void iocp_poke(struct slip_ring *r) {
  struct iocp_state *st = r->be_state;
  PostQueuedCompletionStatus(st->port, 0, IOCP_KEY_POKE, NULL);
}

/* AcceptEx and ConnectEx are not exported by name: Winsock hands out
 * their addresses per socket, through WSAIoctl. Asked once and kept. */
static LPFN_ACCEPTEX iocp_acceptex(SOCKET s) {
  static LPFN_ACCEPTEX fn;
  GUID id = WSAID_ACCEPTEX;
  DWORD got = 0;
  if (fn != NULL) return fn;
  if (WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &id, sizeof(id), &fn, sizeof(fn), &got,
               NULL, NULL) != 0) {
    fn = NULL;
  }
  return fn;
}

static LPFN_CONNECTEX iocp_connectex(SOCKET s) {
  static LPFN_CONNECTEX fn;
  GUID id = WSAID_CONNECTEX;
  DWORD got = 0;
  if (fn != NULL) return fn;
  if (WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &id, sizeof(id), &fn, sizeof(fn), &got,
               NULL, NULL) != 0) {
    fn = NULL;
  }
  return fn;
}

/* ConnectEx refuses a socket that was never bound. The POSIX connect
 * does not, so the shape is kept by binding to the wildcard of the
 * socket's own family first - what a caller would otherwise have to
 * write to get the same behaviour. */
static int iocp_bind_wildcard(SOCKET s) {
  struct sockaddr_storage ss;
  int len = 0;
  WSAPROTOCOL_INFOW info;
  int ilen = (int) sizeof(info);
  if (getsockname(s, (struct sockaddr *) &ss, &(int){(int) sizeof(ss)}) == 0) return 0;
  if (getsockopt(s, SOL_SOCKET, SO_PROTOCOL_INFOW, (char *) &info, &ilen) != 0) return -1;
  memset(&ss, 0, sizeof(ss));
  ss.ss_family = (ADDRESS_FAMILY) info.iAddressFamily;
  len = info.iAddressFamily == AF_INET6 ? (int) sizeof(struct sockaddr_in6)
                                        : (int) sizeof(struct sockaddr_in);
  return bind(s, (struct sockaddr *) &ss, len) == 0 ? 0 : -1;
}

/* Association is per handle and forever; a second call for a handle
 * already on this port fails with ERROR_INVALID_PARAMETER, which is
 * then the association already standing. */
static int iocp_associate(struct iocp_state *st, SOCKET s) {
  if (CreateIoCompletionPort((HANDLE) s, st->port, IOCP_KEY_IO, 0) != NULL) return 0;
  return GetLastError() == ERROR_INVALID_PARAMETER ? 0 : -1;
}

static int iocp_execute(struct slip_ring *r, struct eng_op *op, int *res) {
  struct iocp_state *st = r->be_state;
  const struct io_uring_sqe *s = &op->sqe;
  switch (s->opcode) {
    case IORING_OP_NOP:
      *res = 0;
      return EXEC_DONE;
    case IORING_OP_CLOSE:
      if (closesocket((SOCKET) s->fd) == 0) {
        *res = 0;
      } else if (WSAGetLastError() == WSAENOTSOCK) {
        *res = _close(s->fd) == 0 ? 0 : -EBADF;
      } else {
        *res = -EBADF;
      }
      return EXEC_DONE;
    /* The socket lifecycle: plain Winsock calls that answer at once, so
     * they need no overlapped IO and no port packet. Every field is read
     * where liburing writes it - SOCKET carries the domain in fd, the
     * type in off and the protocol in len; bind and listen carry the
     * address in addr and the backlog in len. */
    case IORING_OP_SOCKET: {
      const SOCKET fd = socket((int) s->fd, (int) s->off, (int) s->len);
      *res = fd == INVALID_SOCKET ? win_err_to_errno((DWORD) WSAGetLastError()) : (int) fd;
      return EXEC_DONE;
    }
    case IORING_OP_BIND: {
      const struct sockaddr *sa = (const struct sockaddr *) (uintptr_t) s->addr;
      const int rc = bind((SOCKET) s->fd, sa, (int) s->off);
      *res = rc == 0 ? 0 : win_err_to_errno((DWORD) WSAGetLastError());
      return EXEC_DONE;
    }
    case IORING_OP_LISTEN: {
      const int rc = listen((SOCKET) s->fd, (int) s->len);
      *res = rc == 0 ? 0 : win_err_to_errno((DWORD) WSAGetLastError());
      return EXEC_DONE;
    }
    case IORING_OP_SHUTDOWN: {
      const int rc = shutdown((SOCKET) s->fd, (int) s->len);
      *res = rc == 0 ? 0 : win_err_to_errno((DWORD) WSAGetLastError());
      return EXEC_DONE;
    }
    /* OPENAT, and the reason it is here at all: a CRT descriptor's
     * handle is not opened FILE_FLAG_OVERLAPPED, and without that flag
     * ReadFile cannot carry an offset or complete through the port. So
     * the engine owns the open. CreateFile takes the flag, and
     * _open_osfhandle wraps the handle as the int a ring speaks in -
     * the handle underneath is still the overlapped one, and
     * _get_osfhandle gives it back for the read.
     *
     * dfd is not honoured: Windows has no openat, and a relative walk
     * from a descriptor is not something CreateFile does. AT_FDCWD is
     * the only value accepted, and anything else is refused by name
     * rather than silently resolved against the wrong directory. */
    case IORING_OP_OPENAT: {
      const char *path = (const char *) (uintptr_t) s->addr;
      const int flags = (int) s->open_flags;
      DWORD access = GENERIC_READ;
      DWORD disposition = OPEN_EXISTING;
      HANDLE h;
      int fd;
      if (path == NULL) {
        *res = -EFAULT;
        return EXEC_DONE;
      }
      if (s->fd != AT_FDCWD) {
        *res = -EOPNOTSUPP;
        return EXEC_DONE;
      }
      if ((flags & O_WRONLY) != 0) access = GENERIC_WRITE;
      else if ((flags & O_RDWR) != 0) access = GENERIC_READ | GENERIC_WRITE;
      if ((flags & O_CREAT) != 0) disposition = (flags & O_EXCL) != 0 ? CREATE_NEW : OPEN_ALWAYS;
      if ((flags & O_TRUNC) != 0) disposition = CREATE_ALWAYS;
      h = CreateFileA(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                      disposition, FILE_FLAG_OVERLAPPED, NULL);
      if (h == INVALID_HANDLE_VALUE) {
        *res = win_err_to_errno(GetLastError());
        return EXEC_DONE;
      }
      fd = _open_osfhandle((intptr_t) h, (flags & O_WRONLY) != 0 ? 0 : _O_RDONLY);
      if (fd < 0) {
        CloseHandle(h);
        *res = -EMFILE;
        return EXEC_DONE;
      }
      *res = fd;
      return EXEC_DONE;
    }
    /* READ and WRITE on what OPENAT above handed out. The offset rides
     * in the OVERLAPPED, which is where a file's position lives on this
     * platform - the descriptor has none of its own. */
    case IORING_OP_READ:
    case IORING_OP_WRITE: {
      const HANDLE h = (HANDLE) _get_osfhandle(s->fd);
      struct iocp_op *w;
      BOOL started;
      if (h == INVALID_HANDLE_VALUE) {
        *res = -EBADF;
        return EXEC_DONE;
      }
      if (CreateIoCompletionPort(h, st->port, IOCP_KEY_IO, 0) == NULL &&
          GetLastError() != ERROR_INVALID_PARAMETER) {
        /* Not an overlapped handle: this descriptor did not come from
         * the open above, and a read on it cannot complete here. */
        *res = -EOPNOTSUPP;
        return EXEC_DONE;
      }
      w = calloc(1, sizeof(*w));
      if (w == NULL) {
        *res = -ENOMEM;
        return EXEC_DONE;
      }
      w->op = op;
      w->sock = (SOCKET) s->fd;
      w->kind = IOCP_KIND_IO;
      w->ov.Offset = (DWORD) (s->off & 0xffffffffu);
      w->ov.OffsetHigh = (DWORD) (s->off >> 32);
      op->be_source = w;
      started = s->opcode == IORING_OP_READ
                    ? ReadFile(h, (void *) (uintptr_t) s->addr, s->len, NULL, &w->ov)
                    : WriteFile(h, (const void *) (uintptr_t) s->addr, s->len, NULL, &w->ov);
      if (!started && GetLastError() != ERROR_IO_PENDING) {
        *res = win_err_to_errno(GetLastError());
        op->be_source = NULL;
        free(w);
        return EXEC_DONE;
      }
      w->next = st->in_flight;
      st->in_flight = w;
      return EXEC_PENDING;
    }
    /* POLL_ADD. A completion port reports what FINISHED, never what is
     * READY, so there is nothing here to ask "is it readable" of. What
     * there is: a recv of ZERO bytes completes when the socket becomes
     * readable and takes nothing off it. That is the readiness, in the
     * one shape this port understands.
     *
     * POLLIN only, and everything else refused by name. POLLOUT has no
     * counterpart - a zero-byte send completes at once whether the
     * socket can take bytes or not, so answering it would be answering
     * a different question. */
    case IORING_OP_POLL_ADD: {
      struct iocp_op *w;
      DWORD flags = 0;
      if ((s->len & IORING_POLL_ADD_MULTI) != 0) {
        *res = -EOPNOTSUPP;
        return EXEC_DONE;
      }
      if ((s->poll32_events & (unsigned) POLLIN) == 0) {
        *res = -EOPNOTSUPP;
        return EXEC_DONE;
      }
      if (iocp_associate(st, (SOCKET) s->fd) != 0) {
        *res = -ENOTSOCK;
        return EXEC_DONE;
      }
      w = calloc(1, sizeof(*w));
      if (w == NULL) {
        *res = -ENOMEM;
        return EXEC_DONE;
      }
      w->op = op;
      w->sock = (SOCKET) s->fd;
      w->kind = IOCP_KIND_POLL;
      w->wb.buf = NULL;
      w->wb.len = 0;
      op->be_source = w;
      if (WSARecv((SOCKET) s->fd, &w->wb, 1, NULL, &flags, &w->ov, NULL) != 0 &&
          WSAGetLastError() != WSA_IO_PENDING) {
        *res = win_err_to_errno((DWORD) WSAGetLastError());
        op->be_source = NULL;
        free(w);
        return EXEC_DONE;
      }
      w->next = st->in_flight;
      st->in_flight = w;
      return EXEC_PENDING;
    }
    /* ACCEPT: AcceptEx wants the new socket made BEFORE it is called, so
     * the wrapper carries it and hands it back as the completion's res.
     * liburing writes the address in addr and the pointer to its length
     * in off - both optional. */
    case IORING_OP_ACCEPT: {
      LPFN_ACCEPTEX acceptex = iocp_acceptex((SOCKET) s->fd);
      struct iocp_op *w;
      SOCKET child;
      DWORD got = 0;
      if (acceptex == NULL || iocp_associate(st, (SOCKET) s->fd) != 0) {
        *res = -ENOTSOCK;
        return EXEC_DONE;
      }
      child = socket(AF_INET, SOCK_STREAM, 0);
      if (child == INVALID_SOCKET) {
        *res = win_err_to_errno((DWORD) WSAGetLastError());
        return EXEC_DONE;
      }
      w = calloc(1, sizeof(*w));
      if (w == NULL) {
        closesocket(child);
        *res = -ENOMEM;
        return EXEC_DONE;
      }
      w->op = op;
      w->sock = (SOCKET) s->fd;
      w->accepted = child;
      w->kind = IOCP_KIND_ACCEPT;
      op->be_source = w;
      if (!acceptex((SOCKET) s->fd, child, w->addrs, 0,
                    sizeof(struct sockaddr_storage) + 16,
                    sizeof(struct sockaddr_storage) + 16, &got, &w->ov) &&
          WSAGetLastError() != WSA_IO_PENDING) {
        *res = win_err_to_errno((DWORD) WSAGetLastError());
        closesocket(child);
        op->be_source = NULL;
        free(w);
        return EXEC_DONE;
      }
      w->next = st->in_flight;
      st->in_flight = w;
      return EXEC_PENDING;
    }
    /* CONNECT: ConnectEx, and a bind first because it insists on one. */
    case IORING_OP_CONNECT: {
      LPFN_CONNECTEX connectex = iocp_connectex((SOCKET) s->fd);
      const struct sockaddr *sa = (const struct sockaddr *) (uintptr_t) s->addr;
      struct iocp_op *w;
      if (connectex == NULL || iocp_associate(st, (SOCKET) s->fd) != 0) {
        *res = -ENOTSOCK;
        return EXEC_DONE;
      }
      if (iocp_bind_wildcard((SOCKET) s->fd) != 0) {
        *res = -EINVAL;
        return EXEC_DONE;
      }
      w = calloc(1, sizeof(*w));
      if (w == NULL) {
        *res = -ENOMEM;
        return EXEC_DONE;
      }
      w->op = op;
      w->sock = (SOCKET) s->fd;
      w->kind = IOCP_KIND_CONNECT;
      op->be_source = w;
      if (!connectex((SOCKET) s->fd, sa, (int) s->off, NULL, 0, NULL, &w->ov) &&
          WSAGetLastError() != WSA_IO_PENDING) {
        *res = win_err_to_errno((DWORD) WSAGetLastError());
        op->be_source = NULL;
        free(w);
        return EXEC_DONE;
      }
      w->next = st->in_flight;
      st->in_flight = w;
      return EXEC_PENDING;
    }
    case IORING_OP_RECV:
    case IORING_OP_SEND: {
      if (iocp_associate(st, (SOCKET) s->fd) != 0) {
        *res = -ENOTSOCK;
        return EXEC_DONE;
      }
      struct iocp_op *w = calloc(1, sizeof(*w));
      if (w == NULL) {
        *res = -ENOMEM;
        return EXEC_DONE;
      }
      w->op = op;
      w->sock = (SOCKET) s->fd;
      w->wb.buf = (char *) (uintptr_t) s->addr;
      w->wb.len = s->len;
      op->be_source = w;
      DWORD flags = 0;
      const int rc = s->opcode == IORING_OP_RECV
                         ? WSARecv((SOCKET) s->fd, &w->wb, 1, NULL, &flags, &w->ov, NULL)
                         : WSASend((SOCKET) s->fd, &w->wb, 1, NULL, 0, &w->ov, NULL);
      if (rc != 0 && WSAGetLastError() != WSA_IO_PENDING) {
        *res = win_err_to_errno((DWORD) WSAGetLastError());
        op->be_source = NULL;
        free(w);
        return EXEC_DONE;
      }
      /* Issued - even an inline success posts its packet to the port,
       * so there is exactly one completion path. */
      w->next = st->in_flight;
      st->in_flight = w;
      return EXEC_PENDING;
    }
    default:
      /* READ/WRITE included, for the reason in the header comment. */
      *res = -EOPNOTSUPP;
      return EXEC_DONE;
  }
}

static int iocp_wait(struct slip_ring *r, struct eng_done *out, unsigned max,
                     int timeout_ms) {
  struct iocp_state *st = r->be_state;
  unsigned n = 0;
  DWORD timeout = timeout_ms < 0 ? INFINITE : (DWORD) timeout_ms;
  while (n < max) {
    DWORD bytes = 0;
    ULONG_PTR key = 0;
    OVERLAPPED *ov = NULL;
    const BOOL ok = GetQueuedCompletionStatus(st->port, &bytes, &key, &ov, timeout);
    if (!ok && ov == NULL) break; /* timeout: the batch is what it is */
    timeout = 0; /* one blocking take, then drain what is already there */
    if (key == IOCP_KEY_POKE) continue; /* the wake itself; zero ops is fine */
    struct iocp_op *w = CONTAINING_RECORD(ov, struct iocp_op, ov);
    struct iocp_op **p = &st->in_flight;
    while (*p != NULL && *p != w) p = &(*p)->next;
    if (*p == w) *p = w->next;
    out[n].op = w->op;
    if (!ok) {
      out[n].res = win_err_to_errno(GetLastError());
      if (w->kind == IOCP_KIND_ACCEPT) closesocket(w->accepted);
    } else {
      switch (w->kind) {
        case IOCP_KIND_ACCEPT:
          /* Until this call the accepted socket has none of the
           * listener's properties - Microsoft says so, and getpeername
           * fails without it. The answer is the socket, not a count. */
          setsockopt(w->accepted, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (const char *) &w->sock,
                     sizeof(w->sock));
          out[n].res = (int) w->accepted;
          break;
        case IOCP_KIND_CONNECT:
          setsockopt(w->sock, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0);
          out[n].res = 0;
          break;
        case IOCP_KIND_POLL:
          /* The zero-byte recv finished, so the socket is readable, and
           * poll answers in poll's own words. */
          out[n].res = POLLIN;
          break;
        default: out[n].res = (int) bytes; break;
      }
    }
    w->op->be_source = NULL;
    free(w);
    n++;
  }
  return (int) n;
}

static const unsigned char iocp_carried_ops[] = {
  IORING_OP_NOP, IORING_OP_RECV, IORING_OP_SEND, IORING_OP_CLOSE,
  IORING_OP_SOCKET, IORING_OP_BIND, IORING_OP_LISTEN, IORING_OP_SHUTDOWN,
  IORING_OP_ACCEPT, IORING_OP_CONNECT, IORING_OP_POLL_ADD,
  IORING_OP_OPENAT, IORING_OP_READ, IORING_OP_WRITE, 255,
};

const struct eng_backend slip_backend_iocp = {
  .name = "iocp",
  .open_ring = iocp_open_ring,
  .close_ring = iocp_close_ring,
  .poke = iocp_poke,
  .execute = iocp_execute,
  .wait = iocp_wait,
  .carried_ops = iocp_carried_ops,
  .arm = NULL,    /* nothing parks here */
  .disarm = NULL,
};

#else
typedef int slip_backend_iocp_is_windows_only;
#endif
