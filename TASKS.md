# slipstreamIO: what is decided, and what is left

Nothing below is running yet except what is listed under **Done**.
This file exists so the reasoning does not have to be rediscovered:
most of these decisions were reached by eliminating alternatives, and
the eliminations are worth more than the conclusions.

## The shape

One API — io_uring's submission/completion model — implemented per
platform. A consumer writes `#include <liburing.h>`, calls the
functions, and never learns which implementation answered.

Three layers, kept strictly apart:

1. **The consumer** decides nothing. No `__has_include`, no define, no
   template parameter, no runtime branch.
2. **This project** decides nothing either. Each implementation is just
   an implementation; none of them probes for the others, includes
   them, or knows they exist.
3. **The packaging layer decides** — `mrbgem.rake` here, the
   distribution in libkqueue's case. It is the only place that looks at
   what the host has.

Mixing 2 and 3 is the mistake to avoid. An implementation that searches
for the thing it replaces is useless to every project but the one it
was written for.

## Done

- `src/liburing.h` — the API over `select(2)`, **in C11**, with both
  halves of the model implemented (see the doctrine below). Provided-
  buffer rings, direct descriptors, linked chains, multishot accept and
  receive, and file IO: `openat`, `read`, `statx`, `close`.

  It is C because real `liburing.h` is C, and a stand-in for a name has
  to be consumable everywhere that name is: `<threads.h>` for the
  threads, `<stdatomic.h>` for the ring cursors (with a `<atomic>` arm
  for C++), plain structs, `static inline` throughout, no destructor —
  `io_uring_queue_exit` owns every teardown step, which is where C put
  it anyway. `test/cconsume.c` is the proof: a `-std=c11` translation
  unit, no feature macros on the command line.

  WHAT THE C SWITCH MADE VISIBLE, and what it cost: `g++` defines
  `_GNU_SOURCE` for you and `gcc -std=c11` does not, so the Linux
  syscalls this file CALLS (`accept4`, `SOCK_CLOEXEC`, `MSG_NOSIGNAL`,
  `statx`) were only ever declared by accident. Real liburing.h never
  notices, because it calls nothing — it fills SQEs and the kernel runs
  them. Ours defines `_GNU_SOURCE` before its first include and says so
  in one `#error` if a libc header got there first. The other find was
  a latent one: `get_sqe` handed out pointers into a `std::vector` that
  could reallocate under the caller. The C version is a fixed array
  allocated once, which is what the kernel's SQ is.

- The ENGINE THREAD and the WORK THREADS — the kernel half of the
  model, and the reason `ring_fd` is a real descriptor. Documented as
  doctrine below; built as: one engine thread per ring owning the
  `select` loop and all readiness ops, a lazily-spawned pool of at most
  four workers for blocking ops, and a completion ring that stays
  strictly single-producer (workers hand results BACK to the engine;
  only the engine writes a CQE and only the engine arms `ring_fd`).

  `IORING_REGISTER_IOWQ_MAX_WORKERS` is deliberately not implemented.
  Nothing has asked for it, and the number it would tune is four.
- `mrbgem.rake` — the layer-3 decision. Takes `mruby-io-uring` as a
  dependency because that gem is the one that knows whether liburing
  built; copies `src/liburing.h` into `include/` only when nothing else
  provides that name. The copy is removed before every check, so a host
  that gains liburing stops being served this one.
- `test/queue.cpp`, `test/wire.cpp`, `test/file.cpp`, `test/cconsume.c`
  — a submission with its completion, a real connection driven only
  through this header, file IO through the pool, and the C consumer.
  The engine-specific proofs live in the first three: `ring_fd` wakes a
  bare `poll()` while the caller is inside no API call at all; the pipe
  is empty again once the ring is; teardown with ops still parked does
  not hang; 5000 completions cross a 4096-entry ring in order (the
  overflow backlog); a socket completion overtakes a blocked file read;
  and a linked chain whose first member is blocking still cancels what
  follows it.

  `make tsan` and `make asan` run all of it under the sanitizers. TSan
  is the verdict that matters and it needed one harness file, NOT a
  change to `src/`: glibc's C11 thread functions do not go through the
  pthread symbols TSan intercepts, so a `thrd_create`d thread crashes
  the runtime — a four-line program proves it, under gcc and clang
  alike. `test/thrd_tsan_shim.c` re-spells the C11 calls as their
  pthread equivalents for the sanitizer build only. It is also the
  shape of the shim macOS will need for real (see task 3).
- `IO_URING_FD_CEILING` — the marker states a PROPERTY, not an
  identity. The header publishes the descriptor ceiling it actually
  has (`FD_SETSIZE`: every fd handed to these functions must stay
  strictly below it, because a connection is a process fd and `select`
  addresses nothing higher), and enforces it against that same name, so
  one number cannot be published and another enforced. ABSENCE is the
  other half: an implementation with no ceiling of its own defines
  nothing, real liburing never will, and a consumer that finds nothing
  has been told its rlimits are the only bound.

  WHY, and why now: `SLIPSTREAM_IO` used to mean "you got slipstreamIO",
  and webmachine-mruby's `raise_nofile` read that name to cap itself at
  `FD_SETSIZE - 1`. True for select, invented for IOCP (no `fd_set` at
  all) and for a macOS build with `_DARWIN_UNLIMITED_SELECT` (heap
  `fd_set`s, a different number) — a second implementation would have
  inherited a wrong answer that already had users. There was exactly
  one consumer, so it cost one `#ifdef` to fix; that is over the moment
  task 1 (Windows: IOCP) lands. `SLIPSTREAM_IO` survives for saying
  WHICH implementation answered — the startup banner that reports
  "correct, not fast" is honest reporting, not a derived limit — and
  nothing else may be hung on it. The consumer side moved with it
  (webmachine-mruby `src/ring.hpp`, `tools/webmachine-server/main.cpp`).
- The waiting half of the API — `io_uring_submit_and_wait_timeout`,
  `io_uring_for_each_cqe`, `io_uring_cq_advance`. What existed before was
  `io_uring_submit_and_wait` plus a `peek_cqe`/`cqe_seen` loop; a driver
  that wants a BOUNDED tick needs a deadline on the wait and a batch walk
  it can stop partway through, and both are the API's job, not the
  driver's.

  `io_uring_submit_and_wait_timeout(ring, cqe_ptr, wait_nr, ts, sigmask)`
  waits for `wait_nr` completions or until `ts` (a `struct
  __kernel_timespec`) runs out. Under select, `ts` IS the timeout
  argument select already takes, so this cost nearly nothing to add.
  Mirrors liburing's own documented behaviour exactly: on success it
  returns the submitted count and sets `*cqe_ptr` to the head completion
  (or NULL if none arrived and `wait_nr` was 0); once the deadline passes
  without `wait_nr` completions, it returns `-ETIME` and clears
  `*cqe_ptr`. `ts == NULL` means what it means in liburing — no deadline
  at all, so this degrades to exactly `io_uring_submit_and_wait`, and
  `-ETIME` can never happen because there is no timeout to run out. An
  empty queue is not a shortcut past the wait: with nothing armed, select
  is still given the remaining time as its timeout (a legal, portable
  sleep with no fds), so a short deadline still takes roughly that long
  before reporting `-ETIME`, the same as it would against a real ring
  with an outstanding but slow request.

  `io_uring_for_each_cqe(ring, head, cqe)` is liburing's own macro form,
  so caller code compiles unchanged: `head` is the caller's loop
  variable, and walking with it never consumes `cq` — only
  `io_uring_cq_advance(ring, nr)` does that, and PARTIALLY, by however
  much `nr` says. A tick cut off mid-batch calls advance with only what
  it actually processed, and the remainder simply stays at the front of
  `cq` for the next tick. This is the reason this task existed: `cq` was
  a `std::deque` already FIFO-ordered front-to-back, so `for_each_cqe`
  could walk it by index without popping and `cq_advance` could pop only
  the walked prefix — no change to the CQ's internal shape was needed,
  only to what operated on it. `io_uring_cqe_seen` now calls
  `io_uring_cq_advance(ring, 1)` internally, exactly liburing's own
  relationship between the two, so single-cqe and batch consumption stay
  one mechanism instead of two.

  Needed by webmachine-mruby's `Webmachine.tick(max_work)` (#116): a
  budget on the WORK, not on the wait, which is why the batch walk had to
  be interruptible and the advance had to be partial.

## Decided, with the reason

These are settled. Reopening one means answering the reason.

### Linux is not a target for this project

Where liburing builds, its symbols are already linked into the process,
so a second implementation of the same names cannot be linked beside
it. That closes the question — including an epoll implementation, which
would otherwise be the most valuable one to have.

The remaining Linux case (liburing cannot be *built* — missing kernel
headers) is exotic, and it is the consumer's business what to do about
it. webmachine-mruby, for one, refuses to build at all there: on Linux
it is io_uring or nothing, because a Linux server that quietly fell
back to select would still be called fast.

### The choice is made at build time, and never at runtime

Every runtime alternative was tried and failed:

- **dlopen** — does not work in a static binary. glibc's static dlopen
  fails; musl's is a stub that always fails.
- **a branch or function pointer inside each operation** — pays for the
  decision on every call, forever, to answer a question settled before
  the first request arrived.
- **a template plus one branch at startup** — costs nothing per call
  and does work, but needs both implementations linked, which the Linux
  case above forbids and which no other platform needs.

And every *build-time probe that runs code* fails for a different
reason: it answers for the build host. A container builds, a server
runs; or the same machine flips `kernel.io_uring_disabled` between the
two. Only "is this header present" is a legitimate build-time question,
because only that describes the build.

### macOS gets select, not kqueue

kqueue on macOS is unreliable in practice (user's operational
experience). It is the native, maintained thing on the real BSDs and
nowhere else.

### The DRIVER loop is the embedder's. The ENGINE is ours.

This replaces "no loop, no thread, no lifetime", and it is worth saying
why, because the old rule was right about the thing it was protecting
and wrong about its own scope.

What it was protecting: the embedder's freedom. No `run` method, no
API-visible thread, no callbacks, nothing that owns the caller's flow of
control. That is untouched and not negotiable — this file never calls
user code.

What it could not have meant: the KERNEL HALF of the API it implements.
io_uring is two halves. One is userspace — submit, walk completions —
and that half was here from the start. The other half runs the
operations, posts the completions, and makes one descriptor readable
when there are any; in real io_uring that half is the kernel, and it is
not optional decoration. It is where `ring_fd` comes from, it is why a
completion can arrive while the caller is computing, and it is what
makes file IO expressible at all. Without a thread of our own, none of
the three is implementable on `select(2)`. That was a structural gap,
not a missing feature, and it was written into the code as
`ring_fd = -1` with a comment explaining that the ring could never be
pollable here. That comment is now gone, because the reason is.

So: **the engine is the implementation's, the driver loop is the
embedder's.** The engine is born in `io_uring_queue_init*`, joined in
`io_uring_queue_exit`, and has no API surface whatsoever — no handle, no
callback, no configuration. A consumer cannot tell it apart from a
kernel except by reading this file.

And it is TWO kinds of thread, because io_uring is two kinds of thread:

- the ENGINE owns the `select` loop and every READINESS operation
  (sockets). It never blocks on work.
- the WORK THREADS are the io-wq analogue: everything BLOCKING —
  `openat`, `read`, `statx`, closing a file descriptor — runs in a
  small, lazily-spawned pool. A file has no readiness for `select` to
  report (it calls every regular file ready, always), so the only
  honest way to offer file IO is to have somebody block on it, and the
  one thread that must never be that somebody is the caller's.

The completion ring stays strictly single-producer across both: workers
never post a CQE. They hand results back to the engine, and the engine
alone writes completions and arms `ring_fd`. That keeps the ring's
memory model exactly the kernel's — one producer, one consumer, no lock
on the completion path — and leaves the `ring_fd` invariant with exactly
one writer.

## Open, in the order they are worth doing

### 1. Pass liburing's own test suite, for the ops we implement

FIRST, before any second implementation, and this is the reason: a
passing suite is what makes a second implementation SAFE. It proves the
API's semantics hold independently of the engine underneath, so kqueue
and IOCP get a fixed target to hit instead of "behaves like the select
one seemed to". It also replaces our own guesses about liburing's
semantics with liburing's own statements — every question this tree has
had to answer by reading documentation (what `-ETIME` means, when a
multishot ends, what `IOSQE_CQE_SKIP_SUCCESS` does) is answered there in
executable form.

Mechanics, as far as they are decided:

- TEST SOURCE, PINNED: the same liburing revision `mruby-io_uring`
  carries as its submodule — `deps/liburing`, today liburing-2.13
  (`e07a859d4b39583c0fe0290730a9d75bccc24b5e`), 242 files under
  `test/`. One revision for the symbols and for the suite, or the two
  drift and the failures stop meaning anything.
- ALLOWLIST, not a full run. Only the test files for operations this
  header implements: nop, accept (and the multishot/direct variants),
  send/recv, poll, link, timeout, buf-ring, openat/read/statx/close,
  fixed files. Every file NOT on the list goes on an exclusion list with
  a NAMED reason — "tests a kernel feature we do not implement", or
  "reaches into liburing's internals / raw `io_uring_enter`". A test we
  cannot pass is either a bug or a documented non-goal; there is no
  third category and no silent skipping.
- liburing's own skip discipline is honoured: `T_EXIT_SKIP` and the
  probe checks mean SKIPPED, and skipped never counts as passed.
- Runs as part of `make test`.

Expect it to push back on the probe surface: several tests ask
`io_uring_get_probe` / the feature flags in order to skip themselves,
and ours answers "yes, everything" to every opcode. That is exactly the
conformance pressure worth having — the probe should start telling the
truth, and the suite is what will say which truth it has to tell.

### 2. Windows: IOCP

The only foreign API that is completion-based, so it is the only one
that maps onto this model rather than being interpreted on top of it.
Submission and completion go across almost unchanged; the parking that
select needs disappears, because the OS holds the operations.

The largest single piece of work here, and the one with the most to
gain.

### 3. macOS: select, past FD_SETSIZE, and a `<threads.h>` of its own

`FD_SETSIZE` is 1024 there, and a server that stops at 1024 connections
is not one. Define `_DARWIN_UNLIMITED_SELECT` before the includes and
allocate `fd_set`s on the heap. Then `IO_URING_FD_CEILING` (Done) stops
being `FD_SETSIZE` and reports the real ceiling — the consumer reads
the property and needs no change, which is the whole point of stating
one.

AND THE FACT THAT COMES WITH IT, stated plainly rather than allowed to
turn into a quiet reversal: **macOS has never shipped `<threads.h>`.**
Apple has not delivered C11 threads to this day; MSVC has had them
since VS 2022 17.8, and glibc has carried `thrd_*` in libc itself since
2.34. So on macOS this header needs a small `thrd_`/`mtx_`/`cnd_` shim
over pthreads — perhaps twenty lines, and `test/thrd_tsan_shim.c`
already shows what it looks like — or Apple ships one. That is a named
part of THIS task. It is not a reason to fall back to pthreads
everywhere: the engine is written against the standard's threads, and
one platform missing a standard header is that platform's gap to fill,
in the same file where its other gaps are filled.

### 4. BSDs: kqueue

Lowest priority: select already serves them correctly, so this is a
performance step, not an enablement one. Readiness-based like select,
so it is a delta on the existing interpreter — only "which descriptors
are ready" changes — not a new implementation. Worth factoring the
interpreter once before writing it, so the third readiness backend does
not copy the first two.

### 5. The operations are still Linux syscalls

`accept4`, `SOCK_CLOEXEC`, `unlinkat`, `MSG_NOSIGNAL`,
`SOCKET_URING_OP_SETSOCKOPT`. The *shape* is portable; the calls are
not. Each one needs its per-platform spelling before any non-Linux
implementation can actually build. Do this as part of task 1 (Windows:
IOCP) rather than speculatively — the second platform is what reveals
which of these are genuinely divergent and which just looked that
way.
