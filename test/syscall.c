/* The three calls liburing makes, answered by us - and the switch that
 * decides who answers. */
#include "slipstream_syscall.h"
#include "uring_available.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/io_uring.h>
#endif

static int fails = 0;

static void ok(int cond, const char *what) {
  printf("%-54s %s\n", what, cond ? "ok" : "FAIL");
  if (!cond) fails++;
}

int main(void) {
#ifdef __linux__
  struct io_uring_params p;

  ok(slipstream_syscall_uses_engine() == (slipstream_uring_available() ? 0 : 1),
     "who answers follows what the kernel said");

  if (!slipstream_syscall_uses_engine()) {
    memset(&p, 0, sizeof(p));
    const int fd = slipstream_io_uring_setup(1, &p);
    ok(fd >= 0, "setup goes to the kernel and a ring comes back");
    ok(p.sq_entries >= 1, "and the kernel filled the params in");
    if (fd >= 0) close(fd);
  }

  /* An operator may want the engine on a kernel that would allow the
   * ring. Asking for it must be enough. */
  slipstream_syscall_set_engine(1);
  ok(slipstream_syscall_uses_engine() == 1, "the engine can be asked for");
  memset(&p, 0, sizeof(p));
  const int ering = slipstream_io_uring_setup(4, &p);
  ok(ering >= 0, "the engine hands out a ring of its own");
  ok(p.sq_entries == 4 && p.cq_entries == 8, "with the entry counts it chose");
  ok(p.sq_off.array != 0 && p.cq_off.cqes != 0,
     "and the offsets liburing will index the blocks by");
  ok(slipstream_io_uring_enter2((unsigned) ering, 0, 0, 0, NULL, 0) == 0,
     "entering an empty ring submits nothing");
  /* Registration is the engine's own tables, and none of that is built. */
  ok(slipstream_io_uring_register((unsigned) ering, 0, NULL, 0) == -EOPNOTSUPP,
     "register says what it cannot do rather than reporting success");
  ok(slipstream_close(ering) == 0, "and the ring goes when it is closed");

  slipstream_syscall_set_engine(0);
  memset(&p, 0, sizeof(p));
  const int back = slipstream_io_uring_setup(1, &p);
  ok(back >= 0, "and back to the kernel when asked the other way");
  if (back >= 0) close(back);
#else
  ok(slipstream_syscall_uses_engine() == 1, "off linux there is only the engine");
#endif
  printf("%s\n", fails ? "FAILURES" : "syscall: ok");
  return fails ? 1 : 0;
}
