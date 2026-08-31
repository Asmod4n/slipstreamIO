/* The fixed-width kernel type names liburing/io_uring.h is written in.
 * On Linux this file is the system's; this one stands in everywhere
 * else, and in the test that proves the shim set compiles liburing.h
 * without any system linux/ header. */
#ifndef SLIPSTREAM_SHIM_LINUX_TYPES_H
#define SLIPSTREAM_SHIM_LINUX_TYPES_H

#include <stdint.h>

typedef uint8_t  __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef int8_t   __s8;
typedef int16_t  __s16;
typedef int32_t  __s32;
typedef int64_t  __s64;

#define __aligned_u64 __u64 __attribute__((aligned(8)))

#endif
