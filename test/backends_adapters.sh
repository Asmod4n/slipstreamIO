#!/bin/sh
# The kqueue and dispatch backends, proven WITHOUT their home platforms:
# libkqueue speaks the kqueue API over epoll on Linux - the same
# packaging move this project makes for liburing.h - and Apple's
# swift-corelibs-libdispatch is GCD's own code, buildable anywhere. The
# backend sources are identical to what a FreeBSD or a Mac compiles;
# only the library underneath differs. The native FreeBSD run lives in
# test/freebsd_vm.sh; a native macOS run needs a Mac.
#
#   LIBDISPATCH_BUILD=/path/to/swift-corelibs-libdispatch test/backends_adapters.sh
set -e

here=$(cd "$(dirname "$0")/.." && pwd)
src=${LIBURING_SRC:-$here/deps/liburing}
if [ ! -f "$src/src/include/liburing/io_uring.h" ]; then
  echo "backends_adapters: no liburing source at $src - skipped"
  echo "  (the engine reads liburing's io_uring.h; set LIBURING_SRC)"
  exit 0
fi
eng="$here/src/slipstream_engine.c $here/src/engine_posix.c $here/src/engine_poll.c $here/src/engine_epoll.c \
     $here/src/engine_kqueue.c $here/src/engine_dispatch.c"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

if kq_flags=$(pkg-config --cflags --libs libkqueue 2>/dev/null); then
  cc -std=c11 -Wall -Wextra -O2 -I"$here/src" -I"$src/src/include" -DSLIPSTREAM_HAVE_LIBKQUEUE $kq_flags \
    -o "$work/backends-kq" "$here/test/backends.c" $eng $kq_flags
  "$work/backends-kq" | grep -A13 '^  kqueue:'
  "$work/backends-kq" > /dev/null
  echo "backends_adapters: kqueue proven through libkqueue"
else
  echo "backends_adapters: no libkqueue - the kqueue adapter scene is skipped"
  echo "  (zypper in libkqueue-devel / apt install libkqueue-dev)"
fi

# swift-corelibs-libdispatch: clang-dialect headers, C++ inside, blocks
# (dispatch_io speaks them), and its static BlocksRuntime rides along.
ld=${LIBDISPATCH_BUILD:-$HOME/swift-corelibs-libdispatch}
if command -v clang >/dev/null 2>&1 && [ -f "$ld/build/src/libdispatch.a" ]; then
  clang -std=c11 -Wall -Wextra -fblocks -O2 -I"$here/src" -I"$src/src/include" -DSLIPSTREAM_HAVE_LIBDISPATCH \
    -I"$ld" -I"$ld/build" -o "$work/backends-dsp" "$here/test/backends.c" $eng \
    "$ld/build/src/libdispatch.a" "$ld/build/src/BlocksRuntime/libBlocksRuntime.a" \
    -lpthread -lstdc++
  "$work/backends-dsp" | grep -A13 '^  dispatch:'
  "$work/backends-dsp" > /dev/null
  echo "backends_adapters: dispatch proven through swift-corelibs-libdispatch"
else
  echo "backends_adapters: no libdispatch build - the dispatch scene is skipped"
  echo "  (clone and build swift-corelibs-libdispatch, or set LIBDISPATCH_BUILD)"
fi
