/* What io_uring.h takes from the kernel's linux/fs.h: the type of the
 * SQE's rw_flags field. */
#ifndef SLIPSTREAM_SHIM_LINUX_FS_H
#define SLIPSTREAM_SHIM_LINUX_FS_H

#include <linux/types.h>

typedef int __kernel_rwf_t;

#endif
