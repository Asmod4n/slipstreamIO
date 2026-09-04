#!/bin/sh
# The IOCP backend, run as a real Windows binary under Wine. Wine's IOCP
# is not Microsoft's, but the calls, the overlapped contract and the
# port semantics are the API's own - the same standard the shim proofs
# hold to: the code that runs here is byte-for-byte what a Windows
# machine compiles. Skipped with words when MinGW or Wine is missing.
#
#   LIBURING_SRC=/path/to/liburing test/backends_wine.sh
set -e

here=$(cd "$(dirname "$0")/.." && pwd)
src=${LIBURING_SRC:-$here/deps/liburing}
if [ ! -f "$src/src/include/liburing/io_uring.h" ]; then
  echo "backends_wine: no liburing source at $src - skipped"
  echo "  (the engine off Linux reads liburing's io_uring.h; set LIBURING_SRC)"
  exit 0
fi
mingw=${MINGW_CC:-x86_64-w64-mingw32-gcc}
if ! command -v "$mingw" >/dev/null 2>&1; then
  echo "backends_wine: no $mingw - skipped (install a mingw-w64 gcc)"
  exit 0
fi
if ! command -v wine >/dev/null 2>&1; then
  echo "backends_wine: no wine - skipped (the exe builds; nothing here runs it)"
  exit 0
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

"$mingw" -std=gnu11 -Wall -Wextra -O2 -I"$here/src" -I"$here/shim/common" \
  -I"$src/src/include" -o "$work/backends.exe" "$here/test/backends.c" \
  "$here"/src/slipstream_engine.c "$here"/src/engine_*.c -lws2_32

WINEDEBUG=-all wine "$work/backends.exe" | grep -A40 '^  iocp:'
WINEDEBUG=-all wine "$work/backends.exe" > /dev/null
echo "backends_wine: iocp proven under Wine"
