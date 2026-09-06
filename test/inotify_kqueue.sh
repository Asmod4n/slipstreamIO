#!/bin/sh
# The EVFILT_VNODE arm, built and run on this host through libkqueue -
# the same API the BSDs and macOS carry, so the arm that ships there is
# the arm that runs here. inotify is the oracle either way: this runs
# test/inotify.c, the same file test/inotify runs.
#
# Skipped with words when there is no libkqueue.
set -e

here=$(cd "$(dirname "$0")/.." && pwd)
inc=""
for dir in /usr/include/kqueue /usr/local/include/kqueue; do
  if [ -f "$dir/sys/event.h" ]; then inc="-I$dir"; break; fi
done
if [ -z "$inc" ]; then
  echo "inotify_kqueue: no libkqueue headers - skipped (install libkqueue-dev)"
  exit 0
fi

out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
${CC:-cc} -std=c11 -Wall -Wextra -O2 -I"$here/src" $inc \
  -DSLIPSTREAM_INOTIFY_KQUEUE \
  -o "$out/inotify_kqueue" "$here/test/inotify.c" "$here/src/slipstream_inotify.c" \
  -lkqueue
"$out/inotify_kqueue"
echo "inotify_kqueue: the EVFILT_VNODE arm proven on this host"
