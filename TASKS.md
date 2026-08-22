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

- `src/liburing.h` — the API over `select(2)`. Submission does the work
  inline for anything that can answer immediately; the rest parks until
  select says ready. Provided-buffer rings, direct descriptors, linked
  chains, multishot accept and receive.
- `mrbgem.rake` — the layer-3 decision. Takes `mruby-io-uring` as a
  dependency because that gem is the one that knows whether liburing
  built; copies `src/liburing.h` into `include/` only when nothing else
  provides that name. The copy is removed before every check, so a host
  that gains liburing stops being served this one.
- `test/queue.cpp`, `test/wire.cpp` — a submission with its completion,
  and a real connection driven only through this header.
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

### No loop, no thread, no lifetime

Whatever drives an implementation belongs to the embedder. This project
provides operations and completions and nothing else. That is not the
same as providing only a bare `submit` — waiting with a deadline, and
walking a batch of completions, are part of the API a driver needs, not
part of the driver (done below).

## Open, in the order they are worth doing

### 1. Windows: IOCP

The only foreign API that is completion-based, so it is the only one
that maps onto this model rather than being interpreted on top of it.
Submission and completion go across almost unchanged; the parking that
select needs disappears, because the OS holds the operations.

The largest single piece of work here, and the one with the most to
gain.

### 2. macOS: select, past FD_SETSIZE

`FD_SETSIZE` is 1024 there, and a server that stops at 1024 connections
is not one. Define `_DARWIN_UNLIMITED_SELECT` before the includes and
allocate `fd_set`s on the heap. Then `IO_URING_FD_CEILING` (Done) stops
being `FD_SETSIZE` and reports the real ceiling — the consumer reads
the property and needs no change, which is the whole point of stating
one.

### 3. BSDs: kqueue

Lowest priority: select already serves them correctly, so this is a
performance step, not an enablement one. Readiness-based like select,
so it is a delta on the existing interpreter — only "which descriptors
are ready" changes — not a new implementation. Worth factoring the
interpreter once before writing it, so the third readiness backend does
not copy the first two.

### 4. The operations are still Linux syscalls

`accept4`, `SOCK_CLOEXEC`, `unlinkat`, `MSG_NOSIGNAL`,
`SOCKET_URING_OP_SETSOCKOPT`. The *shape* is portable; the calls are
not. Each one needs its per-platform spelling before any non-Linux
implementation can actually build. Do this as part of task 1 (Windows:
IOCP) rather than speculatively — the second platform is what reveals
which of these are genuinely divergent and which just looked that
way.
