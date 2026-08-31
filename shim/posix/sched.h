/* liburing.h names cpu_set_t in the io_uring_register_iowq_aff
 * prototypes. glibc's sched.h has it; the BSDs and macOS do not, so this
 * wrapper adds the type after the platform's own sched.h. On FreeBSD and
 * DragonFly it is their native cpuset_t under the glibc name; C11 6.7
 * allows the typedef to repeat where the platform already has it. */
#ifndef SLIPSTREAM_SHIM_SCHED_H
#define SLIPSTREAM_SHIM_SCHED_H

#include_next <sched.h>

#if defined(__FreeBSD__) || defined(__DragonFly__)
#include <sys/param.h>
#include <sys/cpuset.h>
typedef cpuset_t cpu_set_t;
#elif defined(__APPLE__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <stdint.h>
typedef struct {
  uint64_t __bits[16];
} cpu_set_t;
#endif

#endif
