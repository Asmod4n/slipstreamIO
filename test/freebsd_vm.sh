#!/bin/sh
# The shim proof on a REAL FreeBSD, in a VM on this machine - the
# platform branch shim/posix takes for the BSDs (cpuset_t under the
# glibc name) cannot be exercised by a Linux build, so a FreeBSD runs
# test/liburing_h_shims.sh and test/liburing_h_run.c itself, with
# nothing but its base system: cc is clang, sh is sh.
#
# Host side, written for stock openSUSE tools:
#   zypper in qemu-x86 qemu-tools xorriso
# (any of xorriso/mkisofs/genisoimage serves; curl and xz are base.)
#
# The official BASIC-CLOUDINIT VM image boots, nuageinit (FreeBSD's
# base-system cloud-init, Lua - the image ships no python) finds a
# NoCloud seed ISO we build - the same ISO carries the repo and the
# liburing include tree - runs our user-data script at rc.local time,
# which runs the tests, writes every line to the serial port, and
# powers the machine off. This script watches the serial log for the
# verdict. KVM when /dev/kvm is writable, TCG with a longer leash
# otherwise.
#
#   LIBURING_SRC=/path/to/liburing test/freebsd_vm.sh
#   FREEBSD_VERSION=15.0 (default), WORK=dir (default ~/.cache), TIMEOUT=secs
set -e

here=$(cd "$(dirname "$0")/.." && pwd)
src=${LIBURING_SRC:-$here/deps/liburing}
if [ ! -f "$src/src/include/liburing.h" ]; then
  echo "freebsd_vm: no liburing source at $src - the guest needs its include tree"
  echo "  (set LIBURING_SRC to one, or add it under deps/liburing)"
  exit 1
fi

need() {
  command -v "$1" >/dev/null 2>&1 && return 0
  echo "freebsd_vm: missing $1 - $2"
  exit 1
}
need qemu-system-x86_64 "zypper in qemu-x86"
need qemu-img "zypper in qemu-tools"
need curl "zypper in curl"
need xz "zypper in xz"
mkiso=""
for c in "xorriso -as mkisofs" mkisofs genisoimage; do
  command -v "${c%% *}" >/dev/null 2>&1 && { mkiso=$c; break; }
done
[ -n "$mkiso" ] || { echo "freebsd_vm: no ISO tool - zypper in xorriso"; exit 1; }

v=${FREEBSD_VERSION:-15.0}
work=${WORK:-${XDG_CACHE_HOME:-$HOME/.cache}/slipstreamio-freebsd}
mkdir -p "$work"
image="FreeBSD-${v}-RELEASE-amd64-BASIC-CLOUDINIT-ufs.qcow2"

if [ ! -f "$work/$image" ]; then
  url="https://download.freebsd.org/releases/VM-IMAGES/${v}-RELEASE/amd64/Latest/${image}.xz"
  echo "freebsd_vm: fetching $url"
  curl -fL --progress-bar -o "$work/$image.xz" "$url"
  xz -d "$work/$image.xz"
fi

if [ -w /dev/kvm ]; then
  accel="-accel kvm -cpu host"
  timeout=${TIMEOUT:-900}
else
  echo "freebsd_vm: no writable /dev/kvm - TCG, expect minutes not seconds"
  accel="-accel tcg -cpu max"
  timeout=${TIMEOUT:-3600}
fi

run=$(mktemp -d "$work/run.XXXXXX")
# set -e reaches into the trap too: a bare kill with no pid would end
# the trap - and the script's exit code - right there.
trap '[ -n "$qemu_pid" ] && kill $qemu_pid 2>/dev/null; rm -rf "$run"; exit $rc' EXIT
rc=1

# The seed: NoCloud wants user-data and meta-data on a volume labelled
# cidata, and nothing says the same volume may not carry the payload -
# so it does: the repo's shim/ and test/, liburing's include tree, and
# the script the guest runs.
stage="$run/stage"
mkdir -p "$stage/slipstreamio" "$stage/liburing/src"
cp -R "$here/shim" "$here/test" "$here/src" "$stage/slipstreamio/"
cp -R "$src/src/include" "$stage/liburing/src/include"

cat > "$stage/guest.sh" <<'GUEST'
#!/bin/sh
# Runs inside the FreeBSD guest, as root, started by cloud-init. Every
# line goes to the serial port; the last line is the verdict the host
# watches for, and the machine powers off behind it.
exec > /dev/console 2>&1
echo "SLIPSTREAM-FREEBSD: begin on $(uname -sr)"
cp -R /media/slip /tmp/slip
cd /tmp/slip/slipstreamio
# Two proofs: liburing.h through the shims, and the ENGINE itself -
# compiled by the base system's cc against the carried liburing's
# io_uring.h, its kqueue backend driven through the backend scenes.
# threads.h lives in libstdthreads on FreeBSD, hence the -l.
ok=yes
LIBURING_SRC=/tmp/slip/liburing sh test/liburing_h_shims.sh || ok=no
cc -std=c11 -Wall -Wextra -O2 -Isrc -Ishim/common -I/tmp/slip/liburing/src/include \
   -o /tmp/backends test/backends.c src/slipstream_engine.c src/engine_posix.c src/engine_select.c \
   src/engine_epoll.c src/engine_kqueue.c -lstdthreads || ok=no
[ "$ok" = yes ] && /tmp/backends || ok=no
if [ "$ok" = yes ]; then
  echo "SLIPSTREAM-FREEBSD: ok"
else
  echo "SLIPSTREAM-FREEBSD: FAILED"
fi
shutdown -p now
GUEST

cat > "$stage/meta-data" <<EOF
instance-id: slipstreamio-freebsd
local-hostname: slipstream-freebsd
EOF

# NOT cloud-init: FreeBSD's BASIC-CLOUDINIT image carries nuageinit,
# the base system's Lua stand-in, and no python at all - "cloud-init:
# not found" on the machine itself. nuageinit's runcmd takes plain
# strings only (a cloud-config argv list is a Lua table its writer
# cannot concatenate), and there is no bootcmd. The sturdiest contract
# it offers is the oldest one: user-data that starts with #! is made
# executable and run at rc.local time, as root, devices up. So the
# user-data IS the script.
cat > "$stage/user-data" <<'EOF'
#!/bin/sh
exec > /dev/console 2>&1
echo 'SLIPSTREAM-FREEBSD: user-data running'
mkdir -p /media/slip
for d in /dev/iso9660/cidata /dev/iso9660/CIDATA /dev/cd0 /dev/cd1; do
  [ -e "$d" ] || continue
  mount_cd9660 "$d" /media/slip && [ -f /media/slip/guest.sh ] && break
  umount /media/slip 2>/dev/null
done
sh /media/slip/guest.sh
EOF

$mkiso -quiet -o "$run/seed.iso" -V cidata -J -R "$stage" 2>/dev/null

qemu-img create -q -f qcow2 -F qcow2 -b "$work/$image" "$run/disk.qcow2"

# restrict=on starves the image's firstboot freebsd-update: with the
# outside world unreachable its mirror lookup gives up in seconds,
# where fetching metadata and hashing the base system ate a 16-minute
# TCG window, measured. A test guest needs no patches.
qemu-system-x86_64 $accel -machine q35 -m 2048 -smp 2 \
  -drive file="$run/disk.qcow2",if=virtio \
  -drive file="$run/seed.iso",media=cdrom,read-only=on \
  -nic user,model=virtio-net-pci,restrict=on \
  -display none -serial file:"$run/serial.log" -no-reboot &
qemu_pid=$!

echo "freebsd_vm: booted, watching the serial log (timeout ${timeout}s)"
waited=0
while kill -0 $qemu_pid 2>/dev/null; do
  if [ "$waited" -ge "$timeout" ]; then
    echo "freebsd_vm: TIMEOUT after ${timeout}s - the last serial lines:"
    tail -25 "$run/serial.log" 2>/dev/null | sed 's/^/  /'
    exit 1
  fi
  sleep 5
  waited=$((waited + 5))
done
wait $qemu_pid 2>/dev/null || true
qemu_pid=""

sed -n '/SLIPSTREAM-FREEBSD: begin/,$p' "$run/serial.log" | tr -d '\r' | sed 's/^/  /'
if grep -q 'SLIPSTREAM-FREEBSD: ok' "$run/serial.log"; then
  echo "freebsd_vm: ok"
  rc=0
else
  echo "freebsd_vm: FAILED - full serial log kept at $work/serial.log"
  cp "$run/serial.log" "$work/serial.log"
  exit 1
fi
