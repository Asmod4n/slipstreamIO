#ifndef SLIPSTREAM_SYSCALL_H
#define SLIPSTREAM_SYSCALL_H

/* The calls liburing makes into the kernel, answered by us.
 *
 * liburing's own sources - setup.c, queue.c, register.c - do not go
 * through the io_uring_setup/enter/register it EXPORTS; those are
 * wrappers in syscall.c for outside callers. They call __sys_io_uring_*,
 * static inline in src/arch/<arch>/syscall.h. Replacing that one file in
 * a liburing WE build is therefore the whole seam, and it sits entirely
 * inside liburing's own compilation: liburing.h does not include
 * syscall.h, so no consumer of the library ever sees any of this.
 *
 * These must be REACHABLE from liburing's objects, which is what
 * SLIPSTREAM_API is for - a hidden symbol is not one they can bind to.
 *
 * There are THREE and not four: liburing's own __sys_io_uring_enter is
 * nothing but enter2 with the signal set and its size, and _NSIG and
 * sigset_t are POSIX names this header would have to demand a feature
 * macro for. The shim computes it, where signal.h is already in scope.
 */

#include <stddef.h>

#ifndef SLIPSTREAM_API
#ifdef MRB_API
#define SLIPSTREAM_API MRB_API
#elif defined(_WIN32)
#define SLIPSTREAM_API __declspec(dllexport)
#else
#define SLIPSTREAM_API __attribute__((visibility("default")))
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct io_uring_params;

/* Each answers the way the kernel would: the result, or a NEGATED errno.
 * That is liburing's own convention for these - see the arch headers,
 * where every wrapper ends in `(ret < 0) ? -errno : ret`. */
SLIPSTREAM_API int slipstream_io_uring_setup(unsigned int entries,
                                             struct io_uring_params *p);
SLIPSTREAM_API int slipstream_io_uring_enter2(unsigned int fd, unsigned int to_submit,
                                              unsigned int min_complete, unsigned int flags,
                                              void *arg, size_t sz);
SLIPSTREAM_API int slipstream_io_uring_register(unsigned int fd, unsigned int opcode,
                                                const void *arg, unsigned int nr_args);

/* Three more, because when the rings are not the kernel's, liburing's
 * map and close have to reach the memory setup handed out. In kernel
 * mode they are libc, unchanged. */
SLIPSTREAM_API void *slipstream_mmap(void *addr, size_t length, int prot, int flags,
                                     int fd, long long offset);
SLIPSTREAM_API int slipstream_munmap(void *addr, size_t length);
SLIPSTREAM_API int slipstream_close(int fd);

/* Which side answers. Decided once, on first use, by asking the kernel
 * (src/uring_available.h) - never before main, where a refusal would
 * have nobody to tell.
 *
 * Setting it is not only for the tests: an operator with a working ring
 * may still want the engine, and saying so is cheaper than taking the
 * capability away from the process. */
SLIPSTREAM_API int slipstream_syscall_uses_engine(void);
SLIPSTREAM_API void slipstream_syscall_set_engine(int on);

#ifdef __cplusplus
}
#endif

#endif /* SLIPSTREAM_SYSCALL_H */
