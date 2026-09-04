/* The stop signal, as a descriptor. One scene per arm, the same scene:
 * open, raise, see it, read it, close.
 *
 * Linux runs this twice - once on signalfd, once with
 * SLIPSTREAM_SIGNAL_NO_SIGNALFD, which takes the generic POSIX arm. Two
 * binaries, one source, because an arm nobody runs is an arm nobody has
 * checked. The dispatch arm needs a Mac; the console arm runs under Wine
 * (test/signal_wine.sh) and cannot be driven from here, because nothing
 * outside a console session can post CTRL_C_EVENT to it.
 */
/* sigprocmask, kill and sigset_t sit behind these under -std=c11. */
#define _GNU_SOURCE 1
#define _DEFAULT_SOURCE 1

#include "slipstream_signal.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <poll.h>
#include <unistd.h>
#endif

static int failed;

static void check(int ok, const char *what) {
  printf("    %-52s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok) failed = 1;
}

#ifndef _WIN32
static int readable(int fd, int ms) {
  struct pollfd p;
  p.fd = fd;
  p.events = POLLIN;
  p.revents = 0;
  return poll(&p, 1, ms) == 1 && (p.revents & POLLIN) != 0;
}
#endif

int main(void) {
  const int want[2] = {SIGTERM, SIGINT};
  int fd;
  int sig = 0;

#ifndef _WIN32
  /* The contract: blocked first, in every thread. Here there is one. */
  sigset_t m;
  sigemptyset(&m);
  sigaddset(&m, SIGTERM);
  sigaddset(&m, SIGINT);
  sigprocmask(SIG_BLOCK, &m, NULL);
#endif

  printf("  signal:\n");
  fd = slipstream_signal_open(want, 2);
  check(fd >= 0, "a descriptor comes back");
  if (fd < 0) return 1;

  check(slipstream_signal_open(want, 2) == -EEXIST, "a second open refuses");
  check(slipstream_signal_read(fd, &sig) == 0, "nothing waits before a signal");

#ifndef _WIN32
  /* kill(getpid()) and not raise(): raise() aims at the CALLING thread,
   * and a thread-directed signal is pending on that thread alone. The
   * generic arm waits in a thread of its own and would never see it. A
   * TERM from outside is process-directed, which is the case that
   * matters. */
  check(kill(getpid(), SIGTERM) == 0, "the process is sent TERM");
  check(readable(fd, 2000), "the descriptor turns readable");
  check(slipstream_signal_read(fd, &sig) == 1, "one arrival is taken");
  check(sig == SIGTERM, "and it says which signal");
  check(slipstream_signal_read(fd, &sig) == 0, "and nothing waits after it");
#endif

  check(slipstream_signal_close(fd) == 0, "it gives back");
  return failed;
}
