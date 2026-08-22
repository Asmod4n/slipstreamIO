# No build system: the implementation is one header. This only runs the
# tests, which are the proof that the header does what its name says.
#
# LINKING, measured on the build host and not guessed: the engine uses
# C11 <threads.h>, and glibc has carried thrd_* in libc itself since
# 2.34 - so nothing extra is linked here. A host whose thrd_* still live
# in libpthread needs -pthread; that is a property of the host, so it is
# the packaging layer's call (mrbgem.rake), not a flag hardcoded here.
CXXFLAGS ?= -std=c++20 -Wall -Wextra -O2 -Isrc
# -std=c11 with NO feature macro on purpose: the header has to stand up
# for a C consumer on its own terms.
CFLAGS ?= -std=c11 -Wall -Wextra -O2 -Isrc

BINS = test/queue test/wire test/file test/cconsume

test: $(BINS)
	./test/queue && ./test/wire && ./test/file && ./test/cconsume

test/queue: test/queue.cpp src/liburing.h
	$(CXX) $(CXXFLAGS) -o $@ $<

test/wire: test/wire.cpp src/liburing.h
	$(CXX) $(CXXFLAGS) -o $@ $<

test/file: test/file.cpp src/liburing.h
	$(CXX) $(CXXFLAGS) -o $@ $<

# The C half of "one header, both languages". Built with $(CC), not
# $(CXX), and that is the entire point of it.
test/cconsume: test/cconsume.c src/liburing.h
	$(CC) $(CFLAGS) -o $@ $<

# ---- the sanitizers ---------------------------------------------------
#
# TSan is the verdict that matters here: the engine and the caller share
# a completion ring, and "it passed" means nothing without one. It needs
# ONE workaround, and it is a harness file, not a change to src/ - see
# test/thrd_tsan_shim.c for why glibc's C11 threads are invisible to it.
SAN_TSAN = -O1 -g -fsanitize=thread
SAN_ASAN = -O1 -g -fsanitize=address,undefined

tsan: test/thrd_tsan_shim.c src/liburing.h test/queue.cpp test/wire.cpp test/file.cpp test/cconsume.c
	$(CC) $(CFLAGS) $(SAN_TSAN) -c -o test/thrd_tsan_shim.o test/thrd_tsan_shim.c
	for t in queue wire file; do \
	  $(CXX) $(CXXFLAGS) $(SAN_TSAN) -o test/$$t-tsan test/$$t.cpp test/thrd_tsan_shim.o || exit 1; \
	done
	$(CC) $(CFLAGS) $(SAN_TSAN) -o test/cconsume-tsan test/cconsume.c test/thrd_tsan_shim.o
	./test/queue-tsan && ./test/wire-tsan && ./test/file-tsan && ./test/cconsume-tsan

asan: src/liburing.h test/queue.cpp test/wire.cpp test/file.cpp test/cconsume.c
	for t in queue wire file; do \
	  $(CXX) $(CXXFLAGS) $(SAN_ASAN) -o test/$$t-asan test/$$t.cpp || exit 1; \
	done
	$(CC) $(CFLAGS) $(SAN_ASAN) -o test/cconsume-asan test/cconsume.c
	./test/queue-asan && ./test/wire-asan && ./test/file-asan && ./test/cconsume-asan

clean:
	rm -f $(BINS) test/*-tsan test/*-asan test/thrd_tsan_shim.o

.PHONY: test tsan asan clean
