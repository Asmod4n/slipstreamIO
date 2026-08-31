/* MinGW's fcntl.h has the O_ flags but not the openat names. liburing.h
 * uses AT_FDCWD in io_uring_prep_open; the value is the kernel's. */
#ifndef SLIPSTREAM_SHIM_WIN_FCNTL_H
#define SLIPSTREAM_SHIM_WIN_FCNTL_H

#include_next <fcntl.h>

#ifndef AT_FDCWD
#define AT_FDCWD (-100)
#endif

#endif
