# Third party code carried here

## liburing — `deps/liburing`

Dual licensed LGPL-2.1 or MIT by its author, and **this project takes
the MIT half** (`deps/liburing/LICENSE`, Copyright 2020 Jens Axboe).
That is the choice that matters for how it ships: liburing is built
into `liburing.a` and linked into the consuming binary, which under
LGPL-2.1 would carry relinking obligations and under MIT carries the
copyright notice and nothing else.

The two headers that come from the kernel -
`src/include/liburing/io_uring.h` and
`src/include/liburing/io_uring/query.h` - are `GPL-2.0 WITH
Linux-syscall-note` **OR** MIT, and MIT is likewise the half taken. The
syscall note exists precisely so that userspace may speak an ABI
without inheriting the kernel's licence.

## Where this engine's behavior comes from

`src/` reimplements what io_uring does, in userspace, and it may not do
that by reading the kernel. What it is built from:

  - the ABI header carried above, for the shape of every struct and the
    meaning of every flag - MIT, and written to be used this way;
  - liburing's own code, MIT, for what a prep function puts in an SQE;
  - published documentation, io_uring_enter(2) and io_uring_register(2);
  - and, for everything those leave open, ASKING A RUNNING KERNEL:
    test/parity.c submits the same liburing calls to the kernel and to
    the engine and compares the completion streams field for field.

That last one is the important one. Behavior here is settled by
measurement against a black box, not by reading its source, and not
from memory of having read it. Where a kernel cannot answer - an
opcode it predates - the scenario says so and is skipped
(NOT_IN_THIS_KERNEL), or is measured against the plain POSIX call
instead.
