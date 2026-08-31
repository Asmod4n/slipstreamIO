/* struct __kernel_timespec, laid out as the kernel lays it out: 64-bit
 * seconds on every architecture, unlike struct timespec. */
#ifndef SLIPSTREAM_SHIM_LINUX_TIME_TYPES_H
#define SLIPSTREAM_SHIM_LINUX_TIME_TYPES_H

#include <stdint.h>

struct __kernel_timespec {
  int64_t tv_sec;
  long long tv_nsec;
};

#endif
