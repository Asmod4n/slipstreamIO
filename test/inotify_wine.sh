#!/bin/sh
# The ReadDirectoryChangesW arm, as a real Windows binary under Wine.
# It runs test/inotify.c - the same file the other two arms run, with
# inotify as the oracle - so what is proven here is behaviour, not that
# it compiles.
#
# One scene is skipped there and says so: Windows unlinks a directory
# deleted under an open handle at once, and Wine holds the unlink back
# until the handle closes. Nothing can report a deletion that has not
# happened.
#
# Skipped with words when MinGW or Wine is missing.
set -e

here=$(cd "$(dirname "$0")/.." && pwd)
mingw=${MINGW_CC:-x86_64-w64-mingw32-gcc}
if ! command -v "$mingw" >/dev/null 2>&1; then
  echo "inotify_wine: no $mingw - skipped (install a mingw-w64 gcc)"
  exit 0
fi
if ! command -v wine >/dev/null 2>&1; then
  echo "inotify_wine: no wine - skipped (the exe builds; nothing here runs it)"
  exit 0
fi

out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
"$mingw" -std=gnu11 -Wall -Wextra -O2 \
  -I"$here/src" -I"$here/shim/windows" -I"$here/shim/common" \
  -o "$out/inotify.exe" "$here/test/inotify.c" "$here/src/slipstream_inotify.c"
WINEDEBUG=-all wine "$out/inotify.exe"
echo "inotify_wine: the ReadDirectoryChangesW arm proven under Wine"
