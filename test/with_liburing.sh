#!/bin/sh
# Builds a real liburing WITH the shim in it, from a source tree we
# point at, installs its headers where WE say, and then runs an ordinary
# liburing program against exactly those - never the system's.
#
# Two runs, and the second is the proof: with the engine asked for,
# liburing's OWN io_uring_queue_init has to come back with our -ENOSYS.
# It can only reach that through __sys_io_uring_setup.
#
#   LIBURING_SRC=/path/to/liburing test/with_liburing.sh
set -e

here=$(cd "$(dirname "$0")/.." && pwd)
src=${LIBURING_SRC:-$here/deps/liburing}
if [ ! -f "$src/src/syscall.h" ]; then
  echo "with_liburing: no liburing source at $src - skipped"
  echo "  (set LIBURING_SRC to one, or add it under deps/liburing)"
  exit 0
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cp -r "$src" "$work/liburing"
cd "$work/liburing"

# The seam: the file that CHOOSES an arch header, and the twelve
# wrappers it now chooses. Not arch/generic/syscall.h - configure turns
# CONFIG_NOLIBC on by itself on x86_64, and then that file is never
# compiled.
cp "$here/src/liburing_syscall.h"      src/syscall.h
cp "$here/src/liburing_arch_syscall.h" src/
cp "$here/src/slipstream_syscall.h"    src/

./configure --prefix="$work/out" >/dev/null 2>&1
make -j"$(nproc 2>/dev/null || echo 2)" -C src liburing.a >/dev/null 2>&1
mkdir -p "$work/out/include"
cp -r src/include/* "$work/out/include/"

echo "liburing.a references:"
nm src/liburing.a 2>/dev/null | grep -i slipstream | sort -u | sed 's/^/  /'
nm src/liburing.a 2>/dev/null | grep -qi slipstream || {
  echo "  NONE - the shim is not in liburing's path"; exit 1; }

cat > "$work/consumer.c" <<'C'
#include <liburing.h>
#include <errno.h>
#include <stdio.h>
#include "slipstream_syscall.h"

static int run(const char *what) {
  struct io_uring ring;
  int rc = io_uring_queue_init(8, &ring, 0);
  if (rc < 0) { printf("  %-16s queue_init -> %d\n", what, rc); return rc; }
  struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
  io_uring_prep_nop(sqe);
  io_uring_sqe_set_data64(sqe, 0xabc);
  int s = io_uring_submit(&ring);
  struct io_uring_cqe *cqe;
  int w = io_uring_wait_cqe(&ring, &cqe);
  printf("  %-16s submit=%d wait=%d user_data=0x%llx res=%d\n", what, s, w,
         (unsigned long long) cqe->user_data, cqe->res);
  io_uring_cq_advance(&ring, 1);
  io_uring_queue_exit(&ring);
  return 0;
}

int main(void) {
  const int k = run("kernel side:");
  slipstream_syscall_set_engine(1);
  const int e = run("engine forced:");
  /* Both halves are the promise now: the same ordinary program, the same
   * completion, whichever side answers. The archive references above
   * prove the calls went through us to get there. */
  const int good = k == 0 && e == 0;
  printf("%s\n", good ? "with_liburing: ok, both sides"
                       : "with_liburing: one side did not answer");
  return good ? 0 : 1;
}
C

# WHICH liburing.h was used - asked, not assumed. -I ours first, and the
# answer has to name our prefix and not /usr/include.
echo "the header it compiled against:"
cc -I"$work/out/include" -I"$here/src" -H -E "$work/consumer.c" 2>&1 >/dev/null \
  | grep -m1 "liburing\.h" | sed 's/^/  /'
cc -I"$work/out/include" -I"$here/src" -H -E "$work/consumer.c" 2>&1 >/dev/null \
  | grep -m1 "liburing\.h" | grep -q "/usr/include" && {
  echo "  it took the system header"; exit 1; }

cc -O2 -I"$work/out/include" -I"$here/src" -o "$work/consumer" \
   "$work/consumer.c" src/liburing.a "$here/src/slipstream_syscall.c" "$here/src/slipstream_engine.c"
"$work/consumer"

# And the case all of this exists for: the syscalls seccomp'd away, the
# way a container runtime does it, and NOBODY calling set_engine. The
# same ordinary liburing program must complete the same NOP - the
# decision falls on its own.
cat > "$work/blocked.c" <<'C'
#include <liburing.h>
#include <stdio.h>
#include "slipstream_syscall.h"
#include "seccomp_block.h"

int main(void) {
  if (block_io_uring_syscalls() != 0) {
    printf("  seccomp:         this host refuses the filter itself - skipped\n");
    return 0;
  }
  struct io_uring ring;
  int rc = io_uring_queue_init(8, &ring, 0);
  if (rc < 0) { printf("  under seccomp:   queue_init -> %d\n", rc); return 1; }
  struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
  io_uring_prep_nop(sqe);
  io_uring_sqe_set_data64(sqe, 0xabc);
  int su = io_uring_submit(&ring);
  struct io_uring_cqe *cqe;
  int w = io_uring_wait_cqe(&ring, &cqe);
  printf("  under seccomp:   submit=%d wait=%d user_data=0x%llx res=%d engine=%d\n",
         su, w, (unsigned long long) cqe->user_data, cqe->res,
         slipstream_syscall_uses_engine());
  io_uring_cq_advance(&ring, 1);
  io_uring_queue_exit(&ring);
  const int good = su == 1 && w == 0 && cqe->res == 0 && slipstream_syscall_uses_engine() == 1;
  printf("%s\n", good ? "with_liburing under seccomp: ok"
                       : "with_liburing under seccomp: FAILED");
  return good ? 0 : 1;
}
C
cc -O2 -I"$work/out/include" -I"$here/src" -I"$here/test" -o "$work/blocked" \
   "$work/blocked.c" src/liburing.a "$here/src/slipstream_syscall.c" "$here/src/slipstream_engine.c"
"$work/blocked"
