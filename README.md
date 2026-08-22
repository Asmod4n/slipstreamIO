# slipstreamIO

**Carries io_uring's model to every platform that does not have it.**

The model is both halves. One is the API — submission queue entries in,
completion queue entries out. The other is the engine that runs them:
in real io_uring that is the kernel, and here it is one per platform.
An implementation that shipped only the first half would be a shape
without a motor: no completions arriving while you compute, no pollable
ring descriptor, no file IO. So both are implemented.

A consumer writes

```c
#include <liburing.h>
```

and gets the whole model: asynchronous completions that arrive while the
caller is computing, a pollable `ring_fd`, provided-buffer rings, direct
descriptors, linked operations, and file IO — sockets and files through
one API.

## What is here today

`src/liburing.h` — that API, in C11, with `select(2)` as the portable
baseline engine. It is a header and nothing else: no library to link, no
build step, no configuration. It is named for the API it implements,
because that is the only name anyone ever writes — and it deliberately
does not sit on an include path by itself. `mrbgem.rake` copies it into
`include/` exactly when the host has no other `liburing.h`, and leaves
it alone otherwise.

C, not C++, for the same reason it carries that name: real `liburing.h`
is C and is consumable from both, so this one is too. `test/cconsume.c`
compiles it as `-std=c11`; the other tests compile it as C++20.

### The engine, and the work threads

The kernel half, implemented the way the kernel implements it:

- **the engine** — one thread per `struct io_uring`, born in
  `io_uring_queue_init*`, joined in `io_uring_queue_exit`. It owns the
  `select` loop and every readiness operation, posts every completion,
  and is the only thread that makes `ring_fd` readable. It never blocks
  on work.
- **the work threads** — io_uring's io-wq by another name. A file has no
  readiness (`select` calls every regular file ready, always), so
  `openat`, `read`, `statx` and closing a file descriptor are simply
  blocking work, and blocking work goes to a small pool that is spawned
  on demand. A ring that never touches a file never starts one.

Neither has any API surface: no `run` method, no callback, no handle,
nothing to configure. **The driver loop stays entirely the embedder's** —
this header never calls user code. Completions are collected with
`io_uring_submit_and_wait_timeout`, `io_uring_wait_cqe`,
`io_uring_for_each_cqe` and `io_uring_cq_advance`, or by polling
`ring_fd` inside whatever loop the embedder already has.

`ring_fd` is readable if and only if there are completions the caller
has not consumed. That invariant is the point of the whole design, and
`src/liburing.h` documents the two critical sections that hold it up.

## What it does not do

It never looks for the implementation it stands in for. It does not
probe, does not include it, does not decide anything. Choosing between
them is the packaging layer's job — the way `libkqueue` is chosen on
Linux: whoever assembles the build decides what `<liburing.h>` resolves
to, by installing this header under that name or by not installing it.
So consuming code contains no `__has_include`, no define to keep in
sync, no template parameter and no runtime branch. A binary built
against it also stays statically linkable — there is nothing to
`dlopen`, which matters because a static `dlopen` fails on glibc and
musl alike.

The header states a PROPERTY for callers to branch on, never its own
name: `IO_URING_FD_CEILING` — every descriptor handed to these functions
must stay strictly below it, which here is `FD_SETSIZE`, because a
connection is a process fd and `select` addresses nothing higher.
Absence is the other half of the contract: an implementation without a
ceiling of its own — real liburing, for one — defines nothing, and a
caller that finds nothing has been told its rlimits are the only bound.
So the question to ask is `#ifdef IO_URING_FD_CEILING`.

`SLIPSTREAM_IO` is also defined, for saying *which* implementation
answered — a startup banner that reports "correct, not fast" is the
whole use case. Nothing may be derived from it: a name is not a
property, and any limit hung on this one's name would be inherited by
every implementation that comes later.

## Correct, not fast

That is the design goal, stated rather than discovered later.

- every socket operation is readiness plus a classic syscall
- submission is pure handoff: `io_uring_submit` gives the prepped SQEs
  to the engine, in order, and returns
- receive bundles do not exist — one buffer per completion, so the
  dense-fill contract holds trivially
- the pool is four threads and has no knob. It exists to keep the engine
  free, not to parallelise a disk

### Why select is the baseline

`select` is the one readiness primitive that exists everywhere *and* has
been debugged everywhere. macOS' `poll` is permanently broken on several
fd types; `WSAPoll` does not report failed connections (acknowledged by
Microsoft, never fixed). That is why the baseline is `select` and not
`poll`.

Baseline is not the whole plan. `kqueue` on the BSDs and IOCP on Windows
are the native engines this project intends to grow — same API, same
completions, a different motor underneath, which is exactly what having
a model instead of a wrapper is for. `TASKS.md` holds that roadmap and
the reasons.

The operations themselves are still Linux syscalls today. The *shape* is
what is portable, and nothing here pretends the port has been done.

## Tests

```
make test        # C++ consumers and the C one
make tsan        # the same under ThreadSanitizer
make asan        # ... and under AddressSanitizer + UBSan
```

- `test/queue.cpp` — completions, deadlines, partial batch advance, the
  pollable `ring_fd`, the drained pipe, overflow past the ring's size,
  and teardown with operations still parked
- `test/wire.cpp` — a real connection: socket/bind/listen as one linked
  chain, multishot accept onto a direct descriptor, multishot receive
  off a provided-buffer ring, and a send the peer reads back
- `test/file.cpp` — `openat`/`statx`/`read` through the ring, a socket
  completion overtaking a blocked file read, and a linked chain whose
  first member is blocking
- `test/cconsume.c` — the header, compiled as C11

## Licence

Apache-2.0.
