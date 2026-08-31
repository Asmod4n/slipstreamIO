/* See slipstream_syscall.h for what these are and why they are reachable.
 *
 * Two sides, chosen once:
 *
 *   the kernel's, when io_uring is usable for this process - the same
 *   raw calls liburing's own arch headers make, so nothing is emulated
 *   and nothing is in the way;
 *
 *   the engine's, when it is not - and off Linux there was never a
 *   kernel side to begin with.
 */
#include "slipstream_syscall.h"

#include "slipstream_engine.h"
#include "uring_available.h"

#include <errno.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef __linux__
#include <liburing/io_uring.h> /* the carried liburing's ABI, every platform */
#include <sys/syscall.h>
#endif

/* -1 = not asked yet. Asked on first use and not before main: a refusal
 * from a constructor has nobody to tell. */
static int g_engine = -1;

static int engine_chosen(void) {
  if (g_engine < 0) g_engine = slipstream_uring_available() ? 0 : 1;
  return g_engine;
}

int slipstream_syscall_uses_engine(void) { return engine_chosen(); }

void slipstream_syscall_set_engine(int on) { g_engine = on ? 1 : 0; }

#ifdef __linux__
extern long syscall(long, ...);

static int kernel_setup(unsigned int entries, struct io_uring_params *p) {
  const long r = syscall(__NR_io_uring_setup, entries, p);
  return r < 0 ? -errno : (int) r;
}

static int kernel_enter2(unsigned int fd, unsigned int to_submit,
                         unsigned int min_complete, unsigned int flags,
                         void *arg, size_t sz) {
  const long r = syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags, arg, sz);
  return r < 0 ? -errno : (int) r;
}

static int kernel_register(unsigned int fd, unsigned int opcode,
                           const void *arg, unsigned int nr_args) {
  const long r = syscall(__NR_io_uring_register, fd, opcode, arg, nr_args);
  return r < 0 ? -errno : (int) r;
}
#endif

static int engine_setup(unsigned int entries, struct io_uring_params *p) {
  return slipstream_engine_setup(entries, p);
}

/* With IORING_ENTER_EXT_ARG the pointer is a getevents struct carrying a
 * wait timeout; without it, a signal set - the kernel's business, and
 * there is no kernel on this side, so the engine only reads the former. */
static int engine_enter2(unsigned int fd, unsigned int to_submit,
                         unsigned int min_complete, unsigned int flags,
                         void *arg, size_t sz) {
  return slipstream_engine_enter((int) fd, to_submit, min_complete, flags, arg, sz);
}

/* Registration is what the engine carries in its own tables, and none of
 * it is built yet. It says so instead of reporting success it cannot
 * keep. */
static int engine_register(unsigned int fd, unsigned int opcode,
                           const void *arg, unsigned int nr_args) {
  (void) fd; (void) opcode; (void) arg; (void) nr_args;
  return -EOPNOTSUPP;
}

int slipstream_io_uring_setup(unsigned int entries, struct io_uring_params *p) {
#ifdef __linux__
  if (!engine_chosen()) return kernel_setup(entries, p);
#endif
  return engine_setup(entries, p);
}

int slipstream_io_uring_enter2(unsigned int fd, unsigned int to_submit,
                               unsigned int min_complete, unsigned int flags,
                               void *arg, size_t sz) {
#ifdef __linux__
  if (!engine_chosen()) return kernel_enter2(fd, to_submit, min_complete, flags, arg, sz);
#endif
  return engine_enter2(fd, to_submit, min_complete, flags, arg, sz);
}

int slipstream_io_uring_register(unsigned int fd, unsigned int opcode,
                                 const void *arg, unsigned int nr_args) {
#ifdef __linux__
  if (!engine_chosen()) return kernel_register(fd, opcode, arg, nr_args);
#endif
  return engine_register(fd, opcode, arg, nr_args);
}

/* Which calls are the engine's is decided by WHAT is named, not by which
 * mode is on: a ring token goes to the engine, and everything else - an
 * anonymous mapping, a real file, a real descriptor - stays the OS's,
 * in either mode. liburing maps more than rings. */
static int fd_is_ring_token(int fd) { return fd > 0 && (fd & SLIP_RING_TOKEN); }

void *slipstream_mmap(void *addr, size_t length, int prot, int flags, int fd,
                      long long offset) {
  if (fd_is_ring_token(fd)) {
    void *p = slipstream_engine_mmap(length, fd, offset);
    return p ? p : (void *) (intptr_t) -EINVAL;
  }
  void *p = mmap(addr, length, prot, flags, fd, (off_t) offset);
  return (p == MAP_FAILED) ? (void *) (intptr_t) -errno : p;
}

int slipstream_munmap(void *addr, size_t length) {
  /* munmap names an address, not a descriptor, so the engine is asked
   * whether the block is one of its rings; -ENOENT means it is not. */
  if (engine_chosen() && slipstream_engine_munmap(addr, length) == 0) return 0;
  const int rc = munmap(addr, length);
  return rc < 0 ? -errno : rc;
}

int slipstream_close(int fd) {
  if (fd_is_ring_token(fd)) return slipstream_engine_close(fd);
  const int rc = close(fd);
  return rc < 0 ? -errno : rc;
}
