# No build system: the implementation is one header. This only runs the
# tests, which are the proof that the header does what its name says.
CXXFLAGS ?= -std=c++20 -Wall -Wextra -O2 -Isrc

test: test/queue test/wire
	./test/queue && ./test/wire

test/queue: test/queue.cpp src/liburing.h
	$(CXX) $(CXXFLAGS) -o $@ $<

test/wire: test/wire.cpp src/liburing.h
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -f test/queue test/wire

.PHONY: test clean
