/* SPDX-License-Identifier: MIT
 *
 * The replacement for liburing's src/arch/<arch>/syscall.h, copied over
 * that file in a liburing WE build. Twelve wrappers, because that is
 * what liburing's own sources reach for; four of them are ours and the
 * other eight are the plain libc calls they always were.
 *
 * Nothing above the library sees this: liburing.h does not include
 * syscall.h, so a consumer that includes <liburing.h> gets the identical
 * header whether or not this file was in the build.
 *
 * ERR_PTR, IS_ERR and uring_unlikely come from liburing's own syscall.h,
 * which includes this one after defining them.
 */
#ifndef LIBURING_ARCH_SLIPSTREAM_SYSCALL_H
#define LIBURING_ARCH_SLIPSTREAM_SYSCALL_H

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

#include "slipstream_syscall.h"

static inline int __sys_io_uring_register(unsigned int fd, unsigned int opcode,
                                          const void *arg, unsigned int nr_args) {
  return slipstream_io_uring_register(fd, opcode, arg, nr_args);
}

static inline int __sys_io_uring_setup(unsigned int entries,
                                       struct io_uring_params *p) {
  return slipstream_io_uring_setup(entries, p);
}

static inline int __sys_io_uring_enter2(unsigned int fd, unsigned int to_submit,
                                        unsigned int min_complete,
                                        unsigned int flags, void *arg,
                                        size_t sz) {
  return slipstream_io_uring_enter2(fd, to_submit, min_complete, flags, arg, sz);
}

/* The one liburing derives rather than calls: enter is enter2 with the
 * signal set and its size, and here signal.h is in scope. */
static inline int __sys_io_uring_enter(unsigned int fd, unsigned int to_submit,
                                       unsigned int min_complete,
                                       unsigned int flags, sigset_t *sig) {
  return slipstream_io_uring_enter2(fd, to_submit, min_complete, flags, sig,
                                    _NSIG / 8);
}

/* The other eight, unchanged in behaviour from liburing's generic
 * arch header: libc, and a negated errno on failure. */
static inline int __sys_open(const char *pathname, int flags, mode_t mode) {
  int ret = open(pathname, flags, mode);
  return (ret < 0) ? -errno : ret;
}

static inline ssize_t __sys_read(int fd, void *buffer, size_t size) {
  ssize_t ret = read(fd, buffer, size);
  return (ret < 0) ? -errno : ret;
}

static inline void *__sys_mmap(void *addr, size_t length, int prot, int flags,
                               int fd, off_t offset) {
  void *ret = mmap(addr, length, prot, flags, fd, offset);
  return (ret == MAP_FAILED) ? ERR_PTR(-errno) : ret;
}

static inline int __sys_munmap(void *addr, size_t length) {
  int ret = munmap(addr, length);
  return (ret < 0) ? -errno : ret;
}

static inline int __sys_madvise(void *addr, size_t length, int advice) {
  int ret = madvise(addr, length, advice);
  return (ret < 0) ? -errno : ret;
}

static inline int __sys_getrlimit(int resource, struct rlimit *rlim) {
  int ret = getrlimit(resource, rlim);
  return (ret < 0) ? -errno : ret;
}

static inline int __sys_setrlimit(int resource, const struct rlimit *rlim) {
  int ret = setrlimit(resource, rlim);
  return (ret < 0) ? -errno : ret;
}

static inline int __sys_close(int fd) {
  int ret = close(fd);
  return (ret < 0) ? -errno : ret;
}

#endif /* LIBURING_ARCH_SLIPSTREAM_SYSCALL_H */
