/* Windows has no sys/uio.h. liburing.h passes struct iovec pointers into
 * SQEs and never calls readv/writev itself, so the type is all it needs. */
#ifndef SLIPSTREAM_SHIM_SYS_UIO_H
#define SLIPSTREAM_SHIM_SYS_UIO_H

#include <stddef.h>

struct iovec {
  void *iov_base;
  size_t iov_len;
};

#endif
