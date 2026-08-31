/* liburing's configure writes src/include/liburing/compat.h from what it
 * finds on the build host. Off Linux nothing runs configure, so this
 * file stands in - the quoted #include in liburing.h finds a generated
 * one first when it exists, and this one otherwise.
 *
 * The definitions configure would emit on a host that has none of the
 * kernel pieces, minus what the shim's own linux/ headers already carry
 * (__kernel_rwf_t in linux/fs.h, struct __kernel_timespec in
 * linux/time_types.h). */
#ifndef LIBURING_COMPAT_H
#define LIBURING_COMPAT_H

#include <inttypes.h>
#include <linux/time_types.h>

struct open_how {
  uint64_t flags;
  uint64_t mode;
  uint64_t resolve;
};

#define FUTEX_32 2
#define FUTEX_WAITV_MAX 128

struct futex_waitv {
  uint64_t val;
  uint64_t uaddr;
  uint32_t flags;
  uint32_t __reserved;
};

/* The kernel spells it _IO(0x12, 0). The value only matters where a real
 * kernel reads it, and that host generates its own compat.h - so the
 * Linux encoding is written out as the number it is. */
#ifndef BLOCK_URING_CMD_DISCARD
#define BLOCK_URING_CMD_DISCARD 0x1200
#endif

#endif
