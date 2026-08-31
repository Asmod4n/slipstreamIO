# slipstreamIO: what is decided, and what is left

This file exists so the reasoning does not have to be rediscovered:
most of these decisions were reached by eliminating alternatives, and
the eliminations are worth more than the conclusions.

## The shape

There is exactly one `liburing.h`: liburing's own, from a liburing tree
carried at a place we define. A consumer writes `#include <liburing.h>`,
calls the functions, and never learns which side answered. slipstream
supplies what liburing needs from underneath — the twelve `__sys_*`
wrappers its sources call — and nothing above that line. Compatibility
is to liburing, never to the kernel ABI: skipping liburing to talk to
the kernel directly is exactly what io_uring's authors say not to do.

The decision between kernel and engine is made at runtime, by asking
the kernel: attempt `io_uring_setup(2)` at first use. Version numbers
cannot answer it — seccomp and `kernel.io_uring_disabled` say no on
kernels whose version says yes, and a container builds where another
machine runs. This reverses an earlier doctrine of this file ("the
choice is made at build time, and never at runtime"), and the seam is
what dissolved the old objections: both sides are the same symbols in
the same binary — the wrappers — so nothing needs two linked
implementations of liburing's names, and nothing needs dlopen to be
CORRECT. `slipstream_syscall_set_engine(int)` remains for a caller that
wants to choose explicitly; a wrong flag is refused with words.

## Done

- **The seam** — `src/liburing_syscall.h` replaces liburing's
  `src/syscall.h` (the file that *chooses* an arch header; substituting
  `arch/generic/syscall.h` is a false green, configure turns
  `CONFIG_NOLIBC` on by itself on x86_64 and never compiles it).
  Twelve wrappers in `src/liburing_arch_syscall.h`: the four io_uring
  calls plus mmap/munmap/close route through `slipstream_*`;
  open/read/madvise/getrlimit/setrlimit stay libc. Routing is per NAME,
  not per mode — a ring token (`fd & SLIP_RING_TOKEN`) goes to the
  engine, everything else to the OS in either mode, so anonymous
  mappings and real descriptors always reach the kernel.
- **The probe** — `src/uring_available.h`, by attempt, decided once at
  first use and never before `main` (a refusal before main has nobody
  to tell).
- **The engine** — `src/slipstream_engine.c`, the multiplexer behind
  kernel-format SQEs. One rule, the kernel's own: **enter never hangs
  on a single operation** (io_uring is asynchronous in contract; the
  kernel completes inline during enter too, and so does this).
  Immediate ops complete inline; would-block ops are parked and an
  engine thread polls the parked set; regular files — no readiness —
  go to a worker thread. `IOSQE_IO_LINK` failure cancels the rest of
  the chain with `-ECANCELED`, across the worker too. Rings advertise
  `IORING_FEAT_EXT_ARG`, so liburing's `submit_and_wait_timeout` hands
  the deadline to enter instead of prepping a timeout opcode.
  Refusals are exact where liburing branches on them:
  `IORING_SETUP_NO_SQARRAY` answers `-EINVAL` and nothing else, because
  `io_uring_queue_init_try_nosqarr` asks for it FIRST and only that
  answer takes the classic fallback (SQPOLL, SQE128, CQE32, IOPOLL,
  NO_MMAP refuse the same way). Opcodes not carried answer
  `-EOPNOTSUPP` in their own completion — the submit never fails for
  an unknown opcode, because the kernel's does not.
- **liburing.h off Linux** — `shim/`: the four `linux/` headers the
  real `liburing.h` pulls, quoted-include fallbacks for the two files
  liburing's configure generates, `cpu_set_t` for macOS and the BSDs,
  and the POSIX headers Windows lacks. `test/liburing_h_shims.sh`
  proves both stacks — on Linux every shimmed include must resolve
  inside `shim/` (asked with `-H`), and a MinGW cross compile takes the
  Windows set.
- **The proofs** — `test/with_liburing.sh` builds a real liburing WITH
  the seam (hard-fails unless `liburing.a` references `slipstream_*`
  and the consumer compiled against our installed header, not
  `/usr/include`), then one ordinary liburing program completes the
  same NOP on the kernel side, with the engine forced, and under a
  self-applied seccomp filter with nobody calling `set_engine` —
  the environment took the kernel away and the program did not have to
  know. `test/seccomp_block.h` is the filter; each seccomp scene is its
  own process, because the filter is irreversible.

## Decided, with the reason

These are settled. Reopening one means answering the reason.

### Only what liburing needs, and only as private API

The wrappers are exported (`SLIPSTREAM_API`) because liburing's objects
must bind them across a library boundary — hidden symbols cannot be.
They are still private API: no consumer includes our headers, and no
kernel-ABI surface beyond what liburing's own sources call is offered.
No memfd for ring tokens either — Linux-only, and portability is the
point; the token high bit keeps tokens from ever looking like a real
descriptor (token 0 is stdin, measured as an mmap on stdin).

### The engine posts under one mutex, engine and worker both

The old header-only engine kept the CQ strictly single-producer by
handing worker results back to the engine thread. Behind the seam the
CQ is our own calloc'd block and both threads post under `mtx`, with a
release-store on the tail the caller's liburing inlines acquire. The
caller moves the head on its own, so free CQ room is recomputed, never
tracked, and completions the CQ had no room for wait in a backlog that
drains wherever the lock is already held.

### macOS gets dispatch_io, in the completion family

kqueue on macOS is unreliable in practice (operational experience); it
is the native, maintained thing on the real BSDs and nowhere else.
select fell first, then dispatch_source too: dispatch_io DOES what
io_uring does - it runs the IO and reports the outcome - so the macOS
backend sits beside iocp in the completion family instead of
interpreting readiness. The flags objection that briefly picked
dispatch_source resolves cleanly: recv/send WITH flags have no dispatch
spelling and run on the worker, blocking where blocking belongs;
everything else rides real channels, cancellable at close via
DISPATCH_IO_STOP, short reads kept via a low water mark of 1.

### liburing.h's LP64 assumption stays visible

`liburing.h` casts pointers through `unsigned long`, which is 32 bits
on Win64. That is upstream's assumption, not the shim's; it is left as
the warnings it produces, not patched over in a header we promised not
to change - and it is measured, not asserted: the Wine run in
`test/liburing_h_run.c` hands a pointer above 4G to `prep_read` and
finds only its low 32 bits in the SQE. The day upstream fixes the cast,
that check fails and this section goes.

## Open, in the order they are worth doing

### 1. The opcodes webmachine actually preps

cmd_sock, sendmsg, recvmsg_multishot, recv_multishot, accept_direct,
socket_direct, poll_add/update/remove, cancel_fd, bind, listen,
shutdown, statx, unlink, openat2 — plus the register family the server
uses (files_sparse, file_alloc_range, ring_fd, buf_ring). The old
header-only engine implemented most of these semantics once; they move
behind the seam op by op, each with its scene in the tests.

### 2. Carrying liburing, and the packaging step

The build step that copies the two seam files into the carried liburing
tree and installs its headers at the place we define, per consumer gem
(`mruby-io-uring`'s mrbgem.rake is the template). The dlopen of our
built `liburing.so` on the allowed path belongs here too: correctness
never needs it (the wrappers reach the kernel by raw syscall), so it is
a packaging refinement, not a prerequisite.

### 3. Pass liburing's own test suite, for the ops we implement

The first rung of this ladder exists: test/parity.c runs every carried
op through the same liburing calls against the kernel and against the
engine and demands field-identical completion streams - the kernel as
the oracle, divergence printed side by side. It caught the parked
read-on-fd-minus-one on its first outing. liburing's own suite remains
the bigger goal - it also carries semantics no scenario of ours has
thought of yet.

The suite is what makes every next engine SAFE: an allowlist of test
files for carried ops, an exclusion list with a NAMED reason for every
file not on it, liburing's own skip discipline honoured (`T_EXIT_SKIP`
is skipped, never passed). Pinned to the same liburing revision the
consumer carries — one revision for symbols and suite, or failures stop
meaning anything.

### 4. The backends that now exist, and what is still open on them

epoll, kqueue, dispatch and iocp are BUILT (see README, "The
backends"), each proven where it could be: epoll natively, kqueue
natively in the FreeBSD VM and through libkqueue, dispatch against
swift-corelibs-libdispatch, iocp as a MinGW binary under Wine. The
engine core is OS-free and the readiness motors share one POSIX
implementation - the factor-once rule, kept. Open remains:

- Windows file IO: a CRT descriptor's HANDLE is not
  FILE_FLAG_OVERLAPPED, so READ/WRITE answer -EOPNOTSUPP there until
  the engine owns an open path; MSVC also needs #include_next-free
  shims (MinGW is the compiler of record today).
- macOS natively: the dispatch backend and thrd_compat.h's pthreads arm
  have never met a real Mac - thrd_compat carries only the Win32 arm,
  macOS still takes <threads.h> it does not have. Needs forgecore or a
  Mac.
- OpenBSD/NetBSD/DragonFly: the kqueue backend compiles for them by
  guard; no VM run yet - test/freebsd_vm.sh is the template.
- An earlier doctrine fell with this work: "macOS gets select" became
  "macOS gets dispatch_source"; dispatch_io itself cannot carry
  recv/send flags and was refused for that stated reason.

### 5. The operations are still Linux syscalls

`MSG_DONTWAIT`, `SOCK_CLOEXEC`, `statx`, the `SOCKET_URING_OP_*`
commands. The shape is portable; the calls are not. Do the per-platform
spellings as part of the IOCP work rather than speculatively — the
second platform reveals which of these genuinely diverge.
