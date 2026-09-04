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
 * answer at once. READ/WRITE answer -EOPNOTSUPP:
 * a CRT descriptor's HANDLE is not opened FILE_FLAG_OVERLAPPED, so
 * honest file IO here needs its own open path first - said, not
 * half-served. The SQE's fd field is 32 bits by liburing's own ABI;
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
#include <windows.h>
#include <io.h> /* _close: a CRT descriptor is not a SOCKET */
#include <stdlib.h>

struct iocp_op {
  OVERLAPPED ov;
  struct eng_op *op;
  struct iocp_op *next; /* the in-flight list, engine thread only */
  SOCKET sock;
  WSABUF wb;
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
    out[n].res = ok ? (int) bytes : win_err_to_errno(GetLastError());
    w->op->be_source = NULL;
    free(w);
    n++;
  }
  return (int) n;
}

static const unsigned char iocp_carried_ops[] = {
  IORING_OP_NOP, IORING_OP_RECV, IORING_OP_SEND, IORING_OP_CLOSE,
  IORING_OP_SOCKET, IORING_OP_BIND, IORING_OP_LISTEN, IORING_OP_SHUTDOWN, 255,
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
