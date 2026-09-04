/* slipstream_signal.h, four times. See that header for the contract.
 *
 * Three of the four arms end in a pipe or a socket pair, because that is
 * what a caller can poll. Only Linux needs no producer of its own: the
 * kernel already turns the signal into a descriptor.
 */
/* signalfd, sigtimedwait and the sigset calls sit behind these under a
 * strict -std=c11, which is what this Makefile builds with. Declared on
 * the first lines, as engine_posix.c does. */
#define _GNU_SOURCE 1
#define _DEFAULT_SOURCE 1

#include "slipstream_signal.h"

#include <errno.h>
#include <string.h>

/* ---- Linux: the kernel already does it -------------------------------- */
/* SLIPSTREAM_SIGNAL_NO_SIGNALFD takes this arm out of the build, so the
 * generic POSIX arm below can be compiled and proven on a Linux host.
 * The same reason slipstream_syscall_set_engine exists: an arm nobody
 * can run is an arm nobody has checked. */
#if defined(__linux__) && !defined(SLIPSTREAM_SIGNAL_NO_SIGNALFD)

#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

static int g_open;

int slipstream_signal_open(const int *signums, unsigned n) {
  sigset_t m;
  if (signums == NULL || n == 0) return -EINVAL;
  /* One per process on every arm, so a caller writes one thing. Linux
   * could hold several; Windows takes one console handler, and an arm
   * that allows what another refuses is an arm nobody can write against. */
  if (g_open) return -EEXIST;
  sigemptyset(&m);
  for (unsigned i = 0; i < n; i++) {
    if (sigaddset(&m, signums[i]) != 0) return -EINVAL;
  }
  const int fd = signalfd(-1, &m, SFD_CLOEXEC | SFD_NONBLOCK);
  if (fd < 0) return -errno;
  g_open = 1;
  return fd;
}

int slipstream_signal_read(int fd, int *signum) {
  struct signalfd_siginfo si;
  const ssize_t got = read(fd, &si, sizeof(si));
  if (got == (ssize_t) sizeof(si)) {
    if (signum != NULL) *signum = (int) si.ssi_signo;
    return 1;
  }
  if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
  return got < 0 ? -errno : -EIO;
}

int slipstream_signal_close(int fd) {
  g_open = 0;
  return close(fd) == 0 ? 0 : -errno;
}

/* ---- Windows: the console, not a signal -------------------------------- */
#elif defined(_WIN32)

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <signal.h>

/* The producer is a console handler, which Windows runs on a thread it
 * makes for the purpose. It may not block, so it writes one byte and
 * returns. The wire is a loopback pair: Windows has no socketpair(), and
 * a pipe handle is not something the IOCP backend can wait on as a
 * socket. */
static SOCKET g_write = INVALID_SOCKET;
static int g_want[32];
static unsigned g_nwant;

static int wanted(int sig) {
  for (unsigned i = 0; i < g_nwant; i++) {
    if (g_want[i] == sig) return 1;
  }
  return 0;
}

static BOOL WINAPI console_handler(DWORD type) {
  int sig;
  switch (type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT: sig = SIGINT; break;
    /* CLOSE, LOGOFF and SHUTDOWN mean the same thing a TERM means: go
     * away, and you have a short moment to do it in. */
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT: sig = SIGTERM; break;
    default: return FALSE;
  }
  if (!wanted(sig)) return FALSE;
  {
    const char one = (char) sig;
    send(g_write, &one, 1, 0);
  }
  return TRUE;
}

static int loopback_pair(SOCKET sp[2]) {
  struct sockaddr_in a;
  int alen = (int) sizeof(a);
  SOCKET l = socket(AF_INET, SOCK_STREAM, 0);
  SOCKET c;
  SOCKET s;
  if (l == INVALID_SOCKET) return -1;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(l, (struct sockaddr *) &a, sizeof(a)) != 0 || listen(l, 1) != 0 ||
      getsockname(l, (struct sockaddr *) &a, &alen) != 0) {
    closesocket(l);
    return -1;
  }
  c = socket(AF_INET, SOCK_STREAM, 0);
  if (c == INVALID_SOCKET || connect(c, (struct sockaddr *) &a, sizeof(a)) != 0) {
    closesocket(l);
    if (c != INVALID_SOCKET) closesocket(c);
    return -1;
  }
  s = accept(l, NULL, NULL);
  closesocket(l);
  if (s == INVALID_SOCKET) {
    closesocket(c);
    return -1;
  }
  sp[0] = s;
  sp[1] = c;
  return 0;
}

int slipstream_signal_open(const int *signums, unsigned n) {
  SOCKET sp[2];
  WSADATA wsa;
  unsigned long nb = 1;
  if (signums == NULL || n == 0 || n > 32) return -EINVAL;
  if (g_write != INVALID_SOCKET) return -EEXIST;
  WSAStartup(MAKEWORD(2, 2), &wsa);
  if (loopback_pair(sp) != 0) return -EIO;
  ioctlsocket(sp[0], FIONBIO, &nb);
  for (unsigned i = 0; i < n; i++) g_want[i] = signums[i];
  g_nwant = n;
  g_write = sp[1];
  if (!SetConsoleCtrlHandler(console_handler, TRUE)) {
    closesocket(sp[0]);
    closesocket(sp[1]);
    g_write = INVALID_SOCKET;
    g_nwant = 0;
    return -EIO;
  }
  return (int) sp[0];
}

int slipstream_signal_read(int fd, int *signum) {
  char one;
  const int got = recv((SOCKET) fd, &one, 1, 0);
  if (got == 1) {
    if (signum != NULL) *signum = (int) (unsigned char) one;
    return 1;
  }
  if (got < 0 && WSAGetLastError() == WSAEWOULDBLOCK) return 0;
  return got == 0 ? -EIO : -EIO;
}

int slipstream_signal_close(int fd) {
  SetConsoleCtrlHandler(console_handler, FALSE);
  closesocket((SOCKET) fd);
  if (g_write != INVALID_SOCKET) closesocket(g_write);
  g_write = INVALID_SOCKET;
  g_nwant = 0;
  return 0;
}

/* ---- macOS: a dispatch source, and never kqueue ------------------------ */
#elif defined(__APPLE__)

#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

/* kqueue, poll and select are not vendor-supported APIs on this
 * platform, so EVFILT_SIGNAL is out even though it exists.
 *
 * A dispatch source does NOT consume the signal. The default action
 * still runs, and for TERM that ends the process before the handler
 * says anything. So the signal is set to SIG_IGN first: the source sees
 * it either way, and nothing else acts on it. */
static dispatch_source_t g_src[32];
static unsigned g_nsrc;
static int g_write = -1;

int slipstream_signal_open(const int *signums, unsigned n) {
  int fds[2];
  if (signums == NULL || n == 0 || n > 32) return -EINVAL;
  if (g_write >= 0) return -EEXIST;
  if (pipe(fds) != 0) return -errno;
  fcntl(fds[0], F_SETFL, O_NONBLOCK);
  fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  fcntl(fds[1], F_SETFD, FD_CLOEXEC);
  g_write = fds[1];
  for (unsigned i = 0; i < n; i++) {
    const int sig = signums[i];
    dispatch_source_t s;
    signal(sig, SIG_IGN);
    s = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, (uintptr_t) sig, 0,
                               dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0));
    if (s == NULL) {
      close(fds[0]);
      close(fds[1]);
      g_write = -1;
      return -ENOMEM;
    }
    dispatch_source_set_event_handler(s, ^{
      const char one = (char) sig;
      ssize_t ignored = write(g_write, &one, 1);
      (void) ignored;
    });
    dispatch_resume(s);
    g_src[g_nsrc++] = s;
  }
  return fds[0];
}

int slipstream_signal_read(int fd, int *signum) {
  char one;
  const ssize_t got = read(fd, &one, 1);
  if (got == 1) {
    if (signum != NULL) *signum = (int) (unsigned char) one;
    return 1;
  }
  if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
  return got < 0 ? -errno : -EIO;
}

int slipstream_signal_close(int fd) {
  for (unsigned i = 0; i < g_nsrc; i++) {
    dispatch_source_cancel(g_src[i]);
    dispatch_release(g_src[i]);
  }
  g_nsrc = 0;
  if (g_write >= 0) close(g_write);
  g_write = -1;
  return close(fd) == 0 ? 0 : -errno;
}

/* ---- the rest of POSIX: one thread in sigtimedwait --------------------- */
#else

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

/* No signalfd, no dispatch. One thread waits for the signals the caller
 * already blocked and writes what it took. sigtimedwait and not sigwait,
 * so close() can stop the thread with a flag instead of a signal of its
 * own - a stop signal that had to be told apart from the real ones. */
static pthread_t g_thread;
static int g_running;
static int g_stop;
static int g_write = -1;
static sigset_t g_set;

static void *pump(void *unused) {
  (void) unused;
  while (!g_stop) {
    struct timespec wait_for;
    siginfo_t info;
    int got;
    wait_for.tv_sec = 0;
    wait_for.tv_nsec = 100 * 1000 * 1000;
    got = sigtimedwait(&g_set, &info, &wait_for);
    if (got > 0) {
      const char one = (char) got;
      ssize_t ignored = write(g_write, &one, 1);
      (void) ignored;
    }
  }
  return NULL;
}

int slipstream_signal_open(const int *signums, unsigned n) {
  int fds[2];
  if (signums == NULL || n == 0) return -EINVAL;
  if (g_running) return -EEXIST;
  sigemptyset(&g_set);
  for (unsigned i = 0; i < n; i++) {
    if (sigaddset(&g_set, signums[i]) != 0) return -EINVAL;
  }
  if (pipe(fds) != 0) return -errno;
  fcntl(fds[0], F_SETFL, O_NONBLOCK);
  fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  fcntl(fds[1], F_SETFD, FD_CLOEXEC);
  g_write = fds[1];
  g_stop = 0;
  if (pthread_create(&g_thread, NULL, pump, NULL) != 0) {
    close(fds[0]);
    close(fds[1]);
    g_write = -1;
    return -EAGAIN;
  }
  g_running = 1;
  return fds[0];
}

int slipstream_signal_read(int fd, int *signum) {
  char one;
  const ssize_t got = read(fd, &one, 1);
  if (got == 1) {
    if (signum != NULL) *signum = (int) (unsigned char) one;
    return 1;
  }
  if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
  return got < 0 ? -errno : -EIO;
}

int slipstream_signal_close(int fd) {
  if (g_running) {
    g_stop = 1;
    pthread_join(g_thread, NULL);
    g_running = 0;
  }
  if (g_write >= 0) close(g_write);
  g_write = -1;
  return close(fd) == 0 ? 0 : -errno;
}

#endif
