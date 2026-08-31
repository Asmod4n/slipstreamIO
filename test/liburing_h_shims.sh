#!/bin/sh
# liburing's own liburing.h, compiled where there is no Linux to include.
#
# The header pulls <linux/types.h>, <linux/fs.h>, <linux/time_types.h>,
# <linux/swab.h> and two files liburing's configure generates. shim/
# carries stand-ins for all six, plus the POSIX headers Windows lacks.
# Two scenes:
#
#   1. the shim set instead of this host's linux/ headers - every one of
#      the six has to resolve inside shim/, asked with -H and not assumed
#   2. MinGW, a libc with no Linux in it at all - skipped with words when
#      no cross compiler is installed
#
#   LIBURING_SRC=/path/to/liburing test/liburing_h_shims.sh
set -e

here=$(cd "$(dirname "$0")/.." && pwd)
src=${LIBURING_SRC:-$here/deps/liburing}
if [ ! -f "$src/src/include/liburing.h" ]; then
  echo "liburing_h_shims: no liburing source at $src - skipped"
  echo "  (set LIBURING_SRC to one, or add it under deps/liburing)"
  exit 0
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# The include tree alone, minus what configure may have generated into
# it - the fallbacks in shim/liburing must be the ones answering.
cp -r "$src/src/include" "$work/include"
rm -f "$work/include/liburing/compat.h" "$work/include/liburing/io_uring_version.h"

cat > "$work/consume.c" <<'C'
#include <liburing.h>
int sqe_prepped(struct io_uring *ring) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
  if (sqe == NULL) return 0;
  io_uring_prep_nop(sqe);
  io_uring_sqe_set_data64(sqe, 1);
  return 1;
}
C

posix_inc="-I$here/shim/posix -I$here/shim/common -I$work/include"

cc -std=gnu11 -Wall -Wextra -Werror $posix_inc -c "$work/consume.c" -o "$work/consume.o"

for h in linux/types.h linux/fs.h linux/time_types.h linux/swab.h \
         liburing/compat.h liburing/io_uring_version.h; do
  cc -std=gnu11 -H $posix_inc -fsyntax-only "$work/consume.c" 2>&1 \
    | grep -F "$h" | grep -qF "$here/shim/" || {
      echo "liburing_h_shims: $h did not come from shim/"; exit 1; }
done
echo "liburing_h_shims: posix shims answered all six includes"

mingw=${MINGW_CC:-x86_64-w64-mingw32-gcc}
if ! command -v "$mingw" >/dev/null 2>&1; then
  echo "liburing_h_shims: no $mingw - the Windows scene is skipped"
  echo "  (install a mingw-w64 gcc, or point MINGW_CC at one)"
  exit 0
fi

# No -Werror on this side: liburing.h itself casts pointers through
# unsigned long, which is 32 bits on Win64 - upstream's LP64 assumption,
# not the shim's. A warning out of shim/ still fails.
"$mingw" -std=gnu11 -Wall -Wextra \
  -I"$here/shim/windows" -I"$here/shim/common" -I"$work/include" \
  -c "$work/consume.c" -o "$work/consume-win.o" 2> "$work/win.log" || {
    cat "$work/win.log"; echo "liburing_h_shims: MinGW compile FAILED"; exit 1; }
grep -F "$here/shim/" "$work/win.log" && {
  echo "liburing_h_shims: a shim header warned under MinGW"; exit 1; }
echo "liburing_h_shims: MinGW compiled liburing.h with the Windows shims"
