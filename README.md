# slipstreamIO

One API, every major OS. Async socket API for mruby.

## What is here today

`include/slipstreamio.h` — the io_uring submission/completion API,
implemented over `select(2)`. Submission queue entries in, completion
queue entries out, for machines where the real thing is not available.

It is a header, and nothing else: no library to link, no build step, no
configuration.

## What it does not do

It never looks for the implementation it stands in for. It does not
probe, does not include it, does not decide anything. Choosing between
them is the packaging layer's job — the way `libkqueue` is chosen on
Linux: a consumer writes

```c
#include <liburing.h>
```

and calls the functions; whoever assembles the build decides what that
resolves to, by installing this header under that name or by not
installing it. So consuming code contains no `__has_include`, no define
to keep in sync, no template parameter and no runtime branch. A binary
built against it also stays statically linkable — there is nothing to
`dlopen`, which matters because a static `dlopen` fails on glibc and
musl alike.

`SLIPSTREAM_IO` is defined, for the one thing a caller may legitimately
branch on: a few limits differ in kind here. A connection is a process
fd, so the number of them lives under `FD_SETSIZE`.

## Correct, not fast

That is the design goal, stated rather than discovered later.

- every operation is readiness plus a classic syscall
- submission does the work inline: an op that can answer now answers
  inside `io_uring_submit`, the rest parks until `select` says ready
- receive bundles do not exist — one buffer per completion, so the
  dense-fill contract holds trivially
- there is no file IO, on purpose: `select` on a regular file always
  says ready, so a file read would run synchronously and stall the
  caller's loop

### Why select, and not poll/kqueue/epoll

It is the one readiness primitive that exists everywhere *and* has been
debugged everywhere. macOS' `poll` is permanently broken on several fd
types; `WSAPoll` does not report failed connections (acknowledged by
Microsoft, never fixed); `kqueue` is BSD-only and `epoll` Linux-only.
One primitive means one implementation instead of three, with one test
matrix instead of three.

The operations themselves are still Linux syscalls today. The *shape*
is what is portable, and nothing here pretends the port has been done.

## Tests

```
make test
```

- `test/queue.cpp` — a submission and its completion, start to finish
- `test/wire.cpp` — a real connection, driven only through this header:
  socket/bind/listen as one linked chain, multishot accept onto a
  direct descriptor, multishot receive off a provided-buffer ring, and
  a send the peer reads back

## Licence

Apache-2.0.
