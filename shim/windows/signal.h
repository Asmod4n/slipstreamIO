/* MinGW defines _sigset_t always but the POSIX name sigset_t only under
 * _POSIX. liburing.h takes sigset_t pointers, so the name is added after
 * the real header. */
#ifndef SLIPSTREAM_SHIM_WIN_SIGNAL_H
#define SLIPSTREAM_SHIM_WIN_SIGNAL_H

#include_next <signal.h>
#include <sys/types.h>

#ifndef _POSIX
typedef _sigset_t sigset_t;
#endif

#endif
