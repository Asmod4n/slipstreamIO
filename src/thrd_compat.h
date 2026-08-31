/* C11 threads for the engine, everywhere it builds. POSIX hosts have
 * <threads.h> (glibc since 2.34, musl, FreeBSD in libstdthreads); MinGW
 * ships none in either of its thread models - measured on this
 * toolchain, not assumed - and macOS has never shipped one. So the
 * names the engine uses are spelled once over Win32 here, and every
 * other platform takes the real header. The macOS arm is the same shim
 * TASKS.md names, over pthreads, when a Mac build first runs.
 *
 * Only what the engine calls: thrd create/join, mtx plain, cnd with
 * timedwait against TIME_UTC. */
#ifndef SLIPSTREAM_THRD_COMPAT_H
#define SLIPSTREAM_THRD_COMPAT_H

#if defined(_WIN32)

/* windows.h drags winsock 1 in unless told not to, and the iocp backend
 * needs winsock2 - the classic conflict, settled here once. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#include <windows.h>
#include <stdlib.h>
#include <errno.h>
#include <process.h>
#include <time.h>

/* UCRT toolchains have timespec_get; the msvcrt ones do not - measured
 * on this cross gcc. FILETIME counts 100ns ticks from 1601; the offset
 * to the Unix epoch is the constant below. */
#ifndef TIME_UTC
#define TIME_UTC 1
static inline int timespec_get(struct timespec *ts, int base) {
  FILETIME ft;
  ULARGE_INTEGER u;
  GetSystemTimeAsFileTime(&ft);
  u.LowPart = ft.dwLowDateTime;
  u.HighPart = ft.dwHighDateTime;
  const unsigned long long unix100ns = u.QuadPart - 116444736000000000ULL;
  ts->tv_sec = (time_t) (unix100ns / 10000000ULL);
  ts->tv_nsec = (long) (unix100ns % 10000000ULL) * 100;
  return base;
}
#endif

enum { thrd_success = 0, thrd_error = 1, thrd_timedout = 2 };

typedef HANDLE thrd_t;
typedef CRITICAL_SECTION mtx_t;
typedef CONDITION_VARIABLE cnd_t;
typedef int (*thrd_start_t)(void *);
enum { mtx_plain = 0 };

struct thrd_boot {
  thrd_start_t fn;
  void *arg;
};

static inline unsigned __stdcall thrd_compat_run(void *boot) {
  struct thrd_boot b = *(struct thrd_boot *) boot;
  free(boot);
  return (unsigned) b.fn(b.arg);
}

static inline int thrd_create(thrd_t *t, thrd_start_t fn, void *arg) {
  struct thrd_boot *b = malloc(sizeof(*b));
  if (b == NULL) return thrd_error;
  b->fn = fn;
  b->arg = arg;
  const uintptr_t h = _beginthreadex(NULL, 0, thrd_compat_run, b, 0, NULL);
  if (h == 0) {
    free(b);
    return thrd_error;
  }
  *t = (HANDLE) h;
  return thrd_success;
}

static inline int thrd_join(thrd_t t, int *res) {
  WaitForSingleObject(t, INFINITE);
  if (res != NULL) {
    DWORD code = 0;
    GetExitCodeThread(t, &code);
    *res = (int) code;
  }
  CloseHandle(t);
  return thrd_success;
}

static inline int mtx_init(mtx_t *m, int type) {
  (void) type;
  InitializeCriticalSection(m);
  return thrd_success;
}
static inline void mtx_destroy(mtx_t *m) { DeleteCriticalSection(m); }
static inline int mtx_lock(mtx_t *m) { EnterCriticalSection(m); return thrd_success; }
static inline int mtx_unlock(mtx_t *m) { LeaveCriticalSection(m); return thrd_success; }

static inline int cnd_init(cnd_t *c) { InitializeConditionVariable(c); return thrd_success; }
static inline void cnd_destroy(cnd_t *c) { (void) c; }
static inline int cnd_signal(cnd_t *c) { WakeConditionVariable(c); return thrd_success; }
static inline int cnd_broadcast(cnd_t *c) { WakeAllConditionVariable(c); return thrd_success; }
static inline int cnd_wait(cnd_t *c, mtx_t *m) {
  return SleepConditionVariableCS(c, m, INFINITE) ? thrd_success : thrd_error;
}

/* C11 takes an absolute TIME_UTC deadline; the Win32 call takes a span. */
static inline int cnd_timedwait(cnd_t *c, mtx_t *m, const struct timespec *deadline) {
  struct timespec now;
  timespec_get(&now, TIME_UTC);
  long long ms = (deadline->tv_sec - now.tv_sec) * 1000LL +
                 (deadline->tv_nsec - now.tv_nsec) / 1000000LL;
  if (ms < 0) ms = 0;
  if (SleepConditionVariableCS(c, m, (DWORD) ms)) return thrd_success;
  if (GetLastError() != ERROR_TIMEOUT) return thrd_error;
  /* The wait may report timeout a scheduler tick early; C11 says
   * thrd_timedout only once the deadline has actually passed - measured
   * as a real -ETIME arriving ~185ms into a 200ms wait under Wine. An
   * early timeout is a spurious wakeup, and the caller's loop waits the
   * rest. */
  timespec_get(&now, TIME_UTC);
  if (now.tv_sec > deadline->tv_sec ||
      (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec))
    return thrd_timedout;
  return thrd_success;
}

#else
#include <threads.h>
#endif

#endif
