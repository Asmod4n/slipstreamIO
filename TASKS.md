# slipstreamIO: what is decided, and what is left

This file exists so the reasoning does not have to be rediscovered:
most of these decisions were reached by eliminating alternatives, and
the eliminations are worth more than the conclusions.

## The rule that comes before the shape

The kernel is GPL. This engine may not be written from it - not by
reading it, and not from memory of having read it. What may be read:
the ABI header and liburing's own code, both MIT. What settles the
rest: asking a running kernel, in test/parity.c. THIRD_PARTY.md says
this in full, and src/engine_internal.h points at it.

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
machine runs. The seam is what makes it cheap: both sides are the same
symbols in the same binary — the wrappers — so nothing needs two linked
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

- **The stop signal, as a descriptor** — `src/slipstream_signal.c`. A
  server has to hear TERM, and a handler is the wrong way: it runs on
  whichever thread the kernel picks, it may call almost nothing, and it
  needs a second path to wake the loop. Linux turns the signal into a
  descriptor with signalfd; no other platform has one. Four arms give
  every platform that descriptor - signalfd on Linux, a console handler
  and a loopback pair on Windows, a dispatch source on macOS (never
  EVFILT_SIGNAL), one thread in sigtimedwait everywhere else. Proven on
  signalfd, on the generic POSIX arm (which a define selects on Linux),
  and under Wine. The dispatch arm needs a Mac, like the backend does.

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

### macOS gets GCD, in the readiness family

io_uring is a readiness engine wearing completions: it TRIES the op and
only what answers EAGAIN waits for the descriptor. A dispatch source
says exactly that, so the source's handler notes which descriptor woke
and knocks; the op runs on the submitter's thread through the SAME
shared machinery every other backend uses, and macOS therefore answers
the same opcodes, with the same results, as Linux.

What has no readiness to wait for still goes to GCD, never to a thread
of ours: a positioned read or write on a regular file becomes a
dispatch_io channel - completing when the request is DONE, the way
read(2) on a regular file does, not on a low water mark of 1 - and
everything else (connect, openat, a read at the descriptor's own
offset) runs the plain blocking call on the global concurrent queue,
which is where io_uring puts the same ops too. Both come back through
done[] and are handed out by wait on the submitter's thread, so the
core settles chains the ordinary way.

Two sharp edges, both measured:
  - Sources are made when a descriptor's side gains its first waiter
    and cancelled when it loses its last. A FRESH registration is the
    one guaranteed to report readiness that is already there; a source
    kept across an idle period is not.
  - The wait's semaphore is signalled on the TRANSITION to "something
    to collect", never per event. A counting semaphore hands out one
    surplus wakeup per event already answered, and a wakeup with
    nothing behind it is not free: enter has one pass, so it turns a
    caller's deadline into -ETIME on the spot. The deadline scene in
    test/backends.c answered -ETIME immediately instead of at the
    deadline, which is how this was found.

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

Delivered, each with its kernel-parity scenario: socket, bind, listen,
shutdown, accept (single-shot; SOCK_* flags via accept4, spelled out on
macOS), connect (worker - blocking belongs there), sendmsg, recvmsg,
poll_add (the readiness is the answer), poll_remove, async_cancel (by
user_data, by fd, CANCEL_ALL; 0 / -ENOENT / -EALREADY like the kernel),
statx (raw syscall on Linux, an fstatat fallback filling the shim's
struct statx elsewhere), unlinkat, openat and openat2 (worker - open on
a FIFO blocks; off Linux a resolve constraint is refused, not dropped).
The engine speaks the CARRIED liburing's io_uring.h on every platform,
Linux included - this host's /usr/include/linux lacked IORING_OP_BIND.

Since delivered as well, each with its own parity scene: multishot
accept and multishot recv with F_MORE and the buffer id, cmd_sock
(SETSOCKOPT, GETSOCKOPT, GETSOCKNAME), the direct-descriptor variants
and the register family (files_sparse, file_alloc_range, ring_fd,
buf_ring), and MSG_RING in its data form. MSG_RING sits in the CORE and
not in a backend: it posts a CQE on another ring, so all five backends
would do the same thing.

Multishot recvmsg is delivered too, with the layout liburing's own
accessors read and two measured facts in it: res counts the header and
the RESERVED name and control room, not what was written; and EOF keeps
its buffer, where a plain recv gives one back.

Multishot poll is delivered as well: each readiness is a CQE with
F_MORE and the op stays armed, and a cancel ends it with -ECANCELED and
no F_MORE. Emitting and parking again is the whole of it - the ready
loop re-arms anything that parks.

Every opcode webmachine preps is now carried.

Still refused BY NAME, because nothing has measured them: multishot
recvmsg without a provided buffer, and MSG_RING's
fd-passing form (IORING_MSG_SEND_FD), where a wrong answer is one
descriptor in two tables.

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

- Windows file IO is carried now, and only because the OPEN is the
  engine's own: CreateFile with FILE_FLAG_OVERLAPPED, wrapped as an int
  with _open_osfhandle, and READ/WRITE carry their offset in the
  OVERLAPPED because the descriptor has no position of its own. A
  descriptor from anywhere else is refused - the association tells the
  engine it is not an overlapped handle. dfd is not honoured: Windows
  has no openat, so AT_FDCWD is the only value accepted and anything
  else is refused rather than resolved against the wrong directory.
  MSVC still needs #include_next-free shims (MinGW is the compiler of
  record today).
- Windows sockets: iocp carries NOP, RECV, SEND, CLOSE, the socket
  lifecycle (SOCKET, BIND, LISTEN, SHUTDOWN) and ACCEPT and CONNECT
  through AcceptEx and ConnectEx, and POLL_ADD for POLLIN, all proven
  under Wine. A completion port reports what finished and never what is
  ready, so the readiness is a recv of ZERO bytes: it completes when
  the socket becomes readable and takes nothing off it. POLLOUT is
  refused by name - a zero-byte send completes whether the socket can
  take bytes or not, so answering it would answer a different question.
  What is left there: the direct-descriptor and multishot forms of
  accept, multishot poll, and STATX, OPENAT, UNLINKAT, which are file
  calls and wait on the open path above.
- macOS natively: the dispatch backend and thrd_compat.h's pthreads arm
  have never met a real Mac - thrd_compat carries only the Win32 arm,
  macOS still takes <threads.h> it does not have. Needs forgecore or a
  Mac.
- OpenBSD/NetBSD/DragonFly: the kqueue backend compiles for them by
  guard; no VM run yet - test/freebsd_vm.sh is the template.

### 5. The operations are still Linux syscalls

`MSG_DONTWAIT`, `SOCK_CLOEXEC`, `statx`, the `SOCKET_URING_OP_*`
commands. The shape is portable; the calls are not. Do the per-platform
spellings as part of the IOCP work rather than speculatively — the
second platform reveals which of these genuinely diverge.
