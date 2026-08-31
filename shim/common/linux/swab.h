/* liburing.h uses one name from here: __swahw32, swap the two halfwords
 * of a 32-bit value - io_uring_prep_poll_add applies it to the poll mask
 * on big-endian hosts. */
#ifndef SLIPSTREAM_SHIM_LINUX_SWAB_H
#define SLIPSTREAM_SHIM_LINUX_SWAB_H

#include <linux/types.h>

#define __swahw32(x) \
  ((__u32) (((((__u32) (x)) & 0xffff0000u) >> 16) | ((((__u32) (x)) & 0x0000ffffu) << 16)))

#endif
