/* Does the shim stand where liburing puts it?
 *
 * This reproduces what liburing's own src/syscall.h does before it
 * includes the arch file - the ERR_PTR/IS_ERR helpers and <liburing.h> -
 * and then calls every wrapper its sources call. If this compiles and
 * links, the substitution is a valid replacement for
 * src/arch/<arch>/syscall.h.
 *
 * Built with -iquote and NOT -Isrc, on purpose: -Isrc would bend
 * <liburing.h> to slipstream's own header, and this file has to see the
 * REAL one. That is not a detail of the test - it is the rule the whole
 * seam rests on.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <liburing.h>

#ifndef uring_unlikely
#define uring_unlikely(c) __builtin_expect(!!(c), 0)
#endif
static inline void *ERR_PTR(intptr_t n) { return (void *) n; }
static inline bool IS_ERR(const void *p) {
  return uring_unlikely((uintptr_t) p >= (uintptr_t) -4095UL);
}
struct io_uring_params;

#include "liburing_arch_syscall.h"

#include <stdio.h>

int main(void) {
  /* Asked for the engine, so nothing here touches a real ring: what is
   * being tested is that the wrappers EXIST with the shapes liburing
   * calls them in. */
  slipstream_syscall_set_engine(1);

  struct io_uring_params p;
  memset(&p, 0, sizeof(p));
  const int fd = __sys_io_uring_setup(1, &p);
  const int e1 = __sys_io_uring_enter(0, 0, 0, 0, NULL);
  const int e2 = __sys_io_uring_enter2(0, 0, 0, 0, NULL, 0);
  const int rg = __sys_io_uring_register(0, 0, NULL, 0);

  void *m = __sys_mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  const int un = IS_ERR(m) ? -1 : __sys_munmap(m, 4096);
  const int cl = __sys_close(-1);

  const int all_engine = fd == -ENOSYS && e1 == -ENOSYS && e2 == -ENOSYS && rg == -ENOSYS;
  printf("%-54s %s\n", "every __sys_io_uring_* reaches slipstream",
         all_engine ? "ok" : "FAIL");
  printf("%-54s %s\n", "the eight libc wrappers still behave",
         (un == 0 && cl == -EBADF) ? "ok" : "FAIL");
  printf("shim: %s\n", (all_engine && un == 0 && cl == -EBADF) ? "ok" : "FAILURES");
  return (all_engine && un == 0 && cl == -EBADF) ? 0 : 1;
}
