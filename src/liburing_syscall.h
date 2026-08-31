/* SPDX-License-Identifier: MIT
 *
 * The replacement for liburing's src/syscall.h, copied over that file in
 * a liburing WE build. Same helpers, and then our twelve wrappers where
 * liburing would have picked an arch header.
 *
 * IT REPLACES syscall.h AND NOT arch/<arch>/syscall.h, and that is not a
 * preference. liburing's configure turns CONFIG_NOLIBC on BY ITSELF
 * wherever it can - on x86_64 it does - and then arch/x86/syscall.h uses
 * its own __do_syscall macros and never includes arch/generic/syscall.h
 * at all. Substituting the generic file there changes a file that is not
 * compiled: measured, liburing.a came out with no reference to us and a
 * test program went straight to the kernel while appearing to work.
 * Replacing the file that CHOOSES is one file, every arch, and
 * independent of what configure decided.
 *
 * Nothing above the library sees this: liburing.h does not include
 * syscall.h.
 */
#ifndef LIBURING_SYSCALL_H
#define LIBURING_SYSCALL_H

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <liburing.h>

struct io_uring_params;

static inline void *ERR_PTR(intptr_t n) { return (void *) n; }
static inline int PTR_ERR(const void *ptr) { return (int) (intptr_t) ptr; }
static inline bool IS_ERR(const void *ptr) {
  return uring_unlikely((uintptr_t) ptr >= (uintptr_t) -4095UL);
}

#include "liburing_arch_syscall.h"

#endif /* LIBURING_SYSCALL_H */
