#ifndef SLIPSTREAM_ENGINE_H
#define SLIPSTREAM_ENGINE_H

/* The engine's side of the three calls liburing makes, plus the two map
 * wrappers it needs to reach the memory setup handed out. Private: no
 * consumer of liburing ever sees this. */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct io_uring_params;

/* Marks the tokens setup hands out. They travel through liburing as
 * ints and land in mmap and close, so they must not look like a real
 * descriptor - a token of 0 is stdin. */
#define SLIP_RING_TOKEN 0x40000000

/* The readiness backend is a per-process choice: the platform's best by
 * default, another by name for tests and measurements. Refused with
 * -EINVAL for a name this platform does not carry, with -EBUSY once a
 * ring runs - a switch under running rings would strand their parked
 * sets. */
int slipstream_engine_backend_set(const char *name);
const char *slipstream_engine_backend_name(void);

int slipstream_engine_setup(unsigned int entries, struct io_uring_params *p);
int slipstream_engine_enter(int fd, unsigned int to_submit, unsigned int min_complete,
                            unsigned int flags, const void *arg, size_t argsz);
int slipstream_engine_close(int fd);
void *slipstream_engine_mmap(size_t length, int fd, long long offset);
int slipstream_engine_munmap(void *addr, size_t length);

#ifdef __cplusplus
}
#endif

#endif
