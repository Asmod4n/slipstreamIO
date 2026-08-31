/* MinGW's winpthreads ships a sched.h without cpu_set_t; a toolchain
 * without one has nothing to chain to. liburing.h only names the type in
 * prototypes. */
#ifndef SLIPSTREAM_SHIM_WIN_SCHED_H
#define SLIPSTREAM_SHIM_WIN_SCHED_H

#if defined(__has_include_next)
#if __has_include_next(<sched.h>)
#include_next <sched.h>
#endif
#endif

#include <stdint.h>
typedef struct {
  uint64_t __bits[16];
} cpu_set_t;

#endif
