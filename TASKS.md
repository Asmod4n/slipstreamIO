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
same as providing only a bare `submit` — see task 2: waiting with a
deadline, and walking a batch of completions, are part of the API a
driver needs, not part of the driver.

## Open, in the order they are worth doing

### 1. The marker states a PROPERTY, not an identity

`SLIPSTREAM_IO` currently means "you got slipstreamIO", and consumers
derive limits from it — webmachine-mruby caps its connection count at
`FD_SETSIZE` on the strength of that name. That is true for select and
false for everything else. On an IOCP build, or a macOS build with
`_DARWIN_UNLIMITED_SELECT`, the cap would be invented.

The header should say what is true instead: a descriptor ceiling the
consumer can read, or the absence of one. Do this BEFORE the second
implementation exists, or the second implementation inherits a wrong
answer that already has users.

### 2. The waiting half of the API

What exists is `io_uring_submit_and_wait` plus a `peek_cqe`/`cqe_seen`
loop, which is exactly what src/ring.hpp uses today. A driver that
wants a BOUNDED tick needs two more, and they are the API's job, not
the driver's:

- `io_uring_submit_and_wait_timeout` — wait with a deadline
  (`struct __kernel_timespec`), so the caller can cap how long a tick
  may block. Under select this is the timeout argument select already
  takes, so it is nearly free here; the value is that the caller can
  stop asking for it in its own way.
- `io_uring_for_each_cqe` + `io_uring_cq_advance` — walk a batch and
  advance by however much was actually processed, so a tick interrupted
  mid-batch leaves the rest in the queue for the next one.

Needed by webmachine-mruby's `Webmachine.tick(max_work)` (#116): a
budget on the WORK, not on the wait, which means the batch walk has to
be interruptible and the advance has to be partial.

### 3. Windows: IOCP

The only foreign API that is completion-based, so it is the only one
that maps onto this model rather than being interpreted on top of it.
Submission and completion go across almost unchanged; the parking that
select needs disappears, because the OS holds the operations.

The largest single piece of work here, and the one with the most to
gain.

### 4. macOS: select, past FD_SETSIZE

`FD_SETSIZE` is 1024 there, and a server that stops at 1024 connections
is not one. Define `_DARWIN_UNLIMITED_SELECT` before the includes and
allocate `fd_set`s on the heap. Wire it to task 1's property so the
consumer learns the real ceiling.

### 5. BSDs: kqueue

Lowest priority: select already serves them correctly, so this is a
performance step, not an enablement one. Readiness-based like select,
so it is a delta on the existing interpreter — only "which descriptors
are ready" changes — not a new implementation. Worth factoring the
interpreter once before writing it, so the third readiness backend does
not copy the first two.

### 6. The operations are still Linux syscalls

`accept4`, `SOCK_CLOEXEC`, `unlinkat`, `MSG_NOSIGNAL`,
`SOCKET_URING_OP_SETSOCKOPT`. The *shape* is portable; the calls are
not. Each one needs its per-platform spelling before any non-Linux
implementation can actually build. Do this as part of task 3 rather
than speculatively — the second platform is what reveals which of these
are genuinely divergent and which just looked that way.
