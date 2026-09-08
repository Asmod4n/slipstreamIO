#!/bin/sh
# The Windows arm of slipstream_tmpfile, as a real Windows binary under
# Wine. It is the same scene the POSIX arm runs: make the file, write it,
# read it back, and see that the directory holds no entry for it - which
# on Windows is FILE_FLAG_DELETE_ON_CLOSE rather than unlink(2).
#
# Skipped with words when MinGW or Wine is missing.
set -e

here=$(cd "$(dirname "$0")/.." && pwd)
mingw=${MINGW_CC:-x86_64-w64-mingw32-gcc}
if ! command -v "$mingw" >/dev/null 2>&1; then
  echo "tmpfile_wine: no $mingw - skipped (install a mingw-w64 gcc)"
  exit 0
fi
if ! command -v wine >/dev/null 2>&1; then
  echo "tmpfile_wine: no wine - skipped (the exe builds; nothing here runs it)"
  exit 0
fi

out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
"$mingw" -std=gnu11 -Wall -Wextra -O2 \
  -I"$here/src" \
  -o "$out/tmpfile.exe" "$here/test/tmpfile.c" "$here/src/slipstream_tmpfile.c"
WINEDEBUG=-all wine "$out/tmpfile.exe"
echo "tmpfile_wine: the Windows arm proven under Wine"
