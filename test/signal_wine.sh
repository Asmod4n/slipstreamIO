#!/bin/sh
# The console arm of slipstream_signal, as a real Windows binary under
# Wine. What it proves is the wire: the descriptor comes back, a second
# open refuses, an empty read answers 0, and it gives back. The console
# EVENT itself cannot be driven from here - nothing outside a console
# session may post CTRL_C_EVENT to another process - so the mapping from
# CTRL_C_EVENT to SIGINT is read, not run.
#
# Skipped with words when MinGW or Wine is missing.
set -e

here=$(cd "$(dirname "$0")/.." && pwd)
mingw=${MINGW_CC:-x86_64-w64-mingw32-gcc}
if ! command -v "$mingw" >/dev/null 2>&1; then
  echo "signal_wine: no $mingw - skipped (install a mingw-w64 gcc)"
  exit 0
fi
if ! command -v wine >/dev/null 2>&1; then
  echo "signal_wine: no wine - skipped (the exe builds; nothing here runs it)"
  exit 0
fi

out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
"$mingw" -std=gnu11 -Wall -Wextra -O2 \
  -I"$here/src" -I"$here/shim/windows" -I"$here/shim/common" \
  -o "$out/signal.exe" "$here/test/signal.c" "$here/src/slipstream_signal.c" -lws2_32
WINEDEBUG=-all wine "$out/signal.exe"
echo "signal_wine: the console arm proven under Wine"
