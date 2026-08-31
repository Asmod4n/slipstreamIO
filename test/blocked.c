/* The blocked half of the switch, reached the way it is reached in the
 * wild: nobody calls set_engine, seccomp refuses the syscalls, and the
 * decision falls where it falls. */
#include "slipstream_syscall.h"
#include "uring_available.h"
#include "seccomp_block.h"

#include <liburing/io_uring.h> /* the carried liburing's ABI, every platform */
#include <stdio.h>
#include <string.h>

static int fails = 0;

static void ok(int cond, const char *what) {
  printf("%-54s %s\n", what, cond ? "ok" : "FAIL");
  if (!cond) fails++;
}

int main(void) {
  if (block_io_uring_syscalls() != 0) {
    printf("blocked: this host refuses the seccomp filter itself - skipped\n");
    return 0;
  }

  /* The filter took: the raw syscall says EPERM, independent of any of
   * our code. */
  struct io_uring_params p;
  memset(&p, 0, sizeof(p));
  const long raw = syscall(__NR_io_uring_setup, 1, &p);
  ok(raw == -1 && errno == EPERM, "the raw syscall is refused with EPERM");

  ok(slipstream_uring_available() == 0, "the probe says no");
  ok(slipstream_syscall_uses_engine() == 1,
     "and the engine is chosen without anyone asking for it");

  memset(&p, 0, sizeof(p));
  const int ring = slipstream_io_uring_setup(4, &p);
  ok(ring > 0, "setup still hands out a ring - the engine's");
  ok(slipstream_io_uring_enter2((unsigned) ring, 0, 0, 0, NULL, 0) == 0,
     "and it can be entered");
  ok(slipstream_close(ring) == 0, "and closed");

  printf("%s\n", fails ? "FAILURES" : "blocked: ok");
  return fails ? 1 : 0;
}
