# This only runs the tests. What ships is the seam - the two files
# test/with_liburing.sh copies into a liburing tree - the engine behind
# it, and the shim/ headers that let liburing.h compile off Linux.
#
# LINKING, measured on the build host and not guessed: the engine uses
# C11 <threads.h>, and glibc has carried thrd_* in libc itself since
# 2.34 - so nothing extra is linked here. A host whose thrd_* still live
# in libpthread needs -pthread; that is a property of the host, so it is
# the packaging layer's call (mrbgem.rake), not a flag hardcoded here.
#
# -std=c11 with NO feature macro on purpose: the exported headers have to
# stand up for a C consumer on their own terms.
CFLAGS ?= -std=c11 -Wall -Wextra -O2 -Isrc -Ishim/common -I$(LIBURING_SRC)/src/include

# The engine speaks the ABI of the liburing this project carries - its
# liburing/io_uring.h, never the host's /usr/include/linux, which can be
# older than the opcodes the engine serves. Point LIBURING_SRC at a
# liburing source tree; deps/liburing is the carried default.
LIBURING_SRC ?= deps/liburing

# The engine core and every backend the platform guards let through -
# absent ones compile to empty translation units.
ENGINE = src/slipstream_engine.c src/engine_posix.c src/engine_select.c src/engine_epoll.c src/engine_kqueue.c src/engine_dispatch.c src/engine_iocp.c

BINS = test/available test/syscall test/shim test/blocked test/backends \
       test/signal test/signal_posix

abi_header:
	@test -f "$(LIBURING_SRC)/src/include/liburing/io_uring.h" || { \
	  echo "no liburing tree at $(LIBURING_SRC) - the engine needs its io_uring.h"; \
	  echo "  (set LIBURING_SRC to one, or add it under deps/liburing)"; exit 1; }

test: abi_header $(BINS)
	./test/available && ./test/syscall && ./test/shim && ./test/blocked && ./test/backends && ./test/signal && ./test/signal_posix && ./test/backends_adapters.sh && ./test/backends_wine.sh && ./test/signal_wine.sh && ./test/liburing_h_shims.sh && ./test/with_liburing.sh

# The stop signal, twice from one source: once on signalfd, once on the
# generic POSIX arm. An arm nobody runs is an arm nobody has checked.
test/signal: test/signal.c src/slipstream_signal.c src/slipstream_signal.h
	$(CC) $(CFLAGS) -Isrc -o $@ test/signal.c src/slipstream_signal.c

test/signal_posix: test/signal.c src/slipstream_signal.c src/slipstream_signal.h
	$(CC) $(CFLAGS) -Isrc -DSLIPSTREAM_SIGNAL_NO_SIGNALFD -o $@ test/signal.c src/slipstream_signal.c

# The question that runs before liburing exists, so it is built like any
# other C consumer of a header here - and deliberately does not link or
# include liburing, because it is what decides whether liburing is loaded.
test/available: test/available.c src/uring_available.h
	$(CC) $(CFLAGS) -o $@ $<

# The three calls liburing makes, and the switch behind them.
test/syscall: test/syscall.c src/slipstream_syscall.c $(ENGINE) src/engine_internal.h src/slipstream_syscall.h src/uring_available.h
	$(CC) $(CFLAGS) -o $@ test/syscall.c src/slipstream_syscall.c $(ENGINE)

# The blocked half, reached the way the wild reaches it: a seccomp
# filter refuses the io_uring syscalls and the decision falls on its own.
test/blocked: test/blocked.c test/seccomp_block.h src/slipstream_syscall.c $(ENGINE)
	$(CC) $(CFLAGS) -o $@ test/blocked.c src/slipstream_syscall.c $(ENGINE)

# Every backend the platform carries, three scenes each: inline NOP,
# a parked recv a later write completes, -ETIME after a real deadline.
test/backends: test/backends.c $(ENGINE) src/engine_internal.h src/slipstream_engine.h
	$(CC) $(CFLAGS) -o $@ test/backends.c $(ENGINE)

# The shim, compiled where liburing puts it. -iquote and NOT -Isrc: this
# one has to see the REAL <liburing.h>, and -Isrc would hand it ours.
# -std=gnu11 for the same reason liburing builds that way - its own
# sources need the POSIX names.
test/shim: test/shim.c src/liburing_arch_syscall.h src/slipstream_syscall.c $(ENGINE)
	$(CC) -std=gnu11 -Wall -Wextra -O2 -iquote src -I$(LIBURING_SRC)/src/include -Ishim/common -o $@ test/shim.c src/slipstream_syscall.c $(ENGINE)

# The whole thing, end to end: a real liburing built WITH the shim, its
# headers installed where we say, and an ordinary liburing program run
# against exactly those. Needs a liburing source tree - LIBURING_SRC, or
# deps/liburing - and says so and skips when there is none.
.PHONY: with_liburing
with_liburing:
	./test/with_liburing.sh

# liburing's liburing.h compiled against shim/ instead of a Linux:
# the posix stand-ins on this host, MinGW when one is installed.
.PHONY: liburing_h_shims
liburing_h_shims:
	./test/liburing_h_shims.sh

clean:
	rm -f $(BINS)

.PHONY: test clean abi_header
