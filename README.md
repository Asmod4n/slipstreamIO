# slipstreamIO

**Carries io_uring's model to every platform that does not have it.**

There is exactly one `liburing.h`: liburing's own. A consumer writes

```c
#include <liburing.h>
```

against the real header, from a liburing tree carried at a place we
define — and slipstream supplies what that header and liburing's sources
need underneath. Compatibility is to liburing, never to the kernel ABI:
skipping liburing and talking to the kernel directly is exactly what
io_uring's authors say not to do, and this project does not offer it.

## The seam

liburing's own `.c` files reach the kernel through twelve `__sys_*`
wrappers chosen by its `src/syscall.h`. `test/with_liburing.sh` shows
the whole move: that one file is replaced when we build liburing, so
every one of those calls lands in `src/slipstream_syscall.c` first. No
consumer ever sees this — `liburing.h` does not include `syscall.h`.

At first use, slipstream asks the kernel whether io_uring is allowed for
this process — by attempting `io_uring_setup(2)`, not by reading version
numbers, because seccomp and `io_uring_disabled` say no on kernels whose
version says yes. If the kernel answers, every call goes to the kernel.
If it refuses — or there is no Linux underneath at all — the engine in
`src/slipstream_engine.c` answers instead, and the program does not have
to know. `test/blocked.c` proves that under a real seccomp filter.

## The engine

One rule, the same one the kernel keeps: **enter never hangs on a single
operation.** Immediate work completes inline — the kernel completes
inline too; io_uring is asynchronous in contract, not by ceremony. An
operation that would block is parked with the events it waits for, an
engine thread polls the parked set and posts the completion when the
descriptor comes ready. Regular files have no readiness, so file work
goes to a worker thread. `IOSQE_IO_LINK` failure cancels the rest of the
chain with `-ECANCELED`, and rings advertise `IORING_FEAT_EXT_ARG` so
timed waits ride beside `enter` instead of needing a timeout opcode.

Carried today, each held to kernel parity: nop, read, write, recv,
send, sendmsg, recvmsg, accept, connect, socket, bind, listen,
shutdown, close, poll_add, poll_remove, async_cancel (by data, by fd,
all), statx, unlinkat, openat, openat2. Operations the engine does not
carry answer `-EOPNOTSUPP` in their own completion — the submit itself
never fails for an unknown opcode, because that is how the kernel
behaves. The multishot and register families are the named next
stretch.

The API matching is table stakes; the BEHAVIOR has to match, and the
kernel is the oracle: `test/parity.c` runs every scenario twice through
the same liburing calls — kernel answering, then engine forced — and
the completion streams must agree field for field, in order where
io_uring promises order. That harness is what caught a read on fd -1
being parked forever where the kernel says `-EBADF`; every op the
engine gains must land in it.

## liburing.h off Linux

`liburing.h` pulls `<linux/types.h>`, `<linux/fs.h>`,
`<linux/time_types.h>`, `<linux/swab.h>` and two files liburing's
`configure` generates. `shim/` carries stand-ins:

- `shim/common/` — the four `linux/` headers and, as quoted-include
  fallbacks, `liburing/compat.h` and `liburing/io_uring_version.h` for
  trees no configure ever ran in
- `shim/posix/` — macOS and the BSDs: `cpu_set_t` after the platform's
  own `sched.h`
- `shim/windows/` — the POSIX headers Windows lacks: `sys/socket.h`
  (msghdr/cmsghdr; sockaddr stays Winsock's), `sys/uio.h`, `sys/wait.h`,
  plus `AT_FDCWD` and the `sigset_t` name MinGW hides

`test/liburing_h_shims.sh` proves both stacks: on Linux the six shimmed
includes must each resolve inside `shim/` — asked with `-H`, not assumed
— and a MinGW cross compile takes the Windows set. The header's inlines
are then RUN, not only compiled — `test/liburing_h_run.c` drives layout,
preps, cursor math across a mask wrap and the CMSG walk, natively and as
a Windows binary under Wine. liburing.h itself casts pointers through
`unsigned long`, 32 bits on Win64; that is upstream's LP64 assumption,
and the Wine run measures it — a pointer above 4G arrives in the SQE
with only its low 32 bits — rather than patching it over.

## The backends

The engine core - order, chains, the CQ, enter - holds not one OS call
and compiles everywhere, Windows included. How ops RUN is a backend,
chosen per platform and overridable by name until the first ring runs:

Two families. READINESS - the OS says a descriptor came ready, one
shared machine (`src/engine_posix.c`) does everything else:

- `epoll` — Linux, the default there, for the case the engine exists
  for on Linux at all: io_uring refused at runtime
- `kqueue` — the BSDs; proven natively by `test/freebsd_vm.sh` and on
  Linux through libkqueue (`test/backends_adapters.sh`)
- `select` — the floor, always compiled: every platform has it
- `dispatch` — macOS' default: a dispatch source per parked side says
  "ready" and the same shared machine runs the op, so macOS answers
  every opcode Linux does. What has no readiness goes to GCD and never
  to a thread of ours - a positioned file read or write to a
  `dispatch_io` channel, connect/openat to the global concurrent queue.
  Proven against Apple's own swift-corelibs-libdispatch

**macOS takes dispatch, and nothing else.** Apple does not support
kqueue, poll or select as vendor APIs. `backend_default()` answers
`dispatch` there, and `backend_by_name` offers kqueue on the BSDs only.
`engine_kqueue.c` still compiles under `__APPLE__`, for libkqueue, but
nothing on Apple selects it.

COMPLETION - the OS runs the op and reports the outcome, the same
shape io_uring itself has; nothing parks:

- `iocp` — Windows: ops fly as overlapped and the port reports them
  done; proven as a MinGW binary under Wine (`test/backends_wine.sh`),
  with a thrd/mtx/cnd shim because MinGW ships no `<threads.h>`

`test/backends.c` drives every carried backend through the same scenes
- inline NOP, a pending recv that enter must not wait for, `-ETIME`
after a real deadline - and `test/parity.c` holds each of them to the
KERNEL's own answers over 24 scenarios, field for field. select,
epoll, kqueue and dispatch read 24/24. Still named next: overlapped
file IO on Windows (a CRT descriptor's handle is not
`FILE_FLAG_OVERLAPPED`), the socket commands under iocp, and a native
macOS run, which needs a Mac.

## Tests

```
make test
```

- `test/signal.c` — the stop signal as a descriptor, run twice on Linux
  (signalfd, and the generic POSIX arm through
  `SLIPSTREAM_SIGNAL_NO_SIGNALFD`) and once under Wine
  (`test/signal_wine.sh`) for the console arm
- `test/available.c` — the probe: asked twice, answered the same
- `test/syscall.c` — the three exported calls and the switch behind them
- `test/shim.c` — the wrappers, compiled exactly where liburing puts them
- `test/blocked.c` — a seccomp filter refuses the syscalls; the decision
  falls on its own and the ring still answers
- `test/liburing_h_shims.sh` — liburing's header compiled with no Linux
  headers, and under MinGW
- `test/with_liburing.sh` — end to end: a real liburing built with the
  seam, one ordinary program, the same completion on the kernel side,
  with the engine forced, and under seccomp — then `test/parity.c`,
  the kernel-as-oracle differential run over every carried op
- `test/freebsd_vm.sh` — the same shim proof on a REAL FreeBSD, booted
  in a VM with stock tools (qemu, xorriso, curl, xz): the official
  BASIC-CLOUDINIT image, a NoCloud seed whose user-data is the test
  script, the verdict over the serial port. First find on first
  contact: `CMSG_ALIGN` is glibc spelling — FreeBSD hides it, and
  `shim/posix/sys/socket.h` now supplies the platform's own alignment

## Licence

Apache-2.0.
