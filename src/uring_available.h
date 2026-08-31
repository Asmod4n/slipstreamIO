#ifndef SLIPSTREAM_URING_AVAILABLE_H
#define SLIPSTREAM_URING_AVAILABLE_H

/* Does io_uring work FOR THIS PROCESS, right now?
 *
 * Asked before liburing is loaded, so it cannot use it: this is the
 * question whose answer decides whether liburing is loaded at all. It is
 * an ATTEMPT and not a version check, because the answer does not follow
 * from the kernel version:
 *
 *   ENOSYS   the kernel has no io_uring at all
 *   EPERM    /proc/sys/kernel/io_uring_disabled is 1 (CAP_SYS_ADMIN only)
 *            or 2 (off), or seccomp refuses the syscall - both happen on
 *            kernels that carry the feature, and both are common in
 *            containers
 *   EFAULT / anything else: not usable here either
 *
 * Off Linux there is nothing to ask. There is no io_uring, and there is
 * no liburing.so to load even where its header exists, so the answer is
 * known while compiling.
 */

#ifdef __linux__

#include <liburing/io_uring.h> /* the carried liburing's ABI, every platform */
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

/* Declared here and not taken from <unistd.h>: this repo compiles with
 * -std=c11 and NO feature macro on purpose, and glibc only declares
 * syscall() under _GNU_SOURCE or _DEFAULT_SOURCE. Asking a consumer to
 * define one to include a header would be the header failing to stand up
 * on its own terms. */
extern long syscall(long, ...);

static inline int slipstream_uring_available(void) {
  struct io_uring_params p;
  memset(&p, 0, sizeof(p));
  /* One entry: the smallest ring the kernel will hand out, and it is
   * closed again immediately. Nothing is mapped - a descriptor coming
   * back at all is the whole answer. */
  const long fd = syscall(__NR_io_uring_setup, 1u, &p);
  if (fd < 0) return 0;
  close((int) fd);
  return 1;
}

#else

static inline int slipstream_uring_available(void) { return 0; }

#endif

#endif /* SLIPSTREAM_URING_AVAILABLE_H */
