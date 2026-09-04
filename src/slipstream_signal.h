/* A stop signal, as something the engine can wait for.
 *
 * A server has to hear TERM. A handler is the wrong way to hear it: it
 * runs on whichever thread the kernel picks, it may call almost nothing,
 * and it needs a second path to wake the loop it interrupts. Linux
 * solves this with signalfd - the signal becomes a descriptor, and the
 * loop reads it like any other. No other platform has signalfd.
 *
 * This gives every platform that descriptor. What produces it differs;
 * what a caller does with it does not:
 *
 *   int fd = slipstream_signal_open(want, 2);
 *   io_uring_prep_poll_add(sqe, fd, POLLIN);
 *
 * THE CALLER BLOCKS THE SIGNALS FIRST, in every thread, before it makes
 * a thread. A thread inherits the mask of the thread that makes it, and
 * a signal reaches any thread that does not block it. On Windows there
 * is nothing to block and the call is not needed.
 *
 * Only asynchronous signals work this way. SIGSEGV and SIGFPE must reach
 * the thread that caused them, so no descriptor can collect them.
 *
 * A signal a thread sends to ITSELF - raise(3) - is pending on that
 * thread alone. signalfd on that same thread reads it; the generic POSIX
 * arm waits in a thread of its own and never sees it. Send to the
 * PROCESS - kill(getpid(), sig) - and every arm answers. A signal from
 * outside is process-directed already, which is the case this exists
 * for.
 */
#ifndef SLIPSTREAM_SIGNAL_H
#define SLIPSTREAM_SIGNAL_H

#ifndef SLIPSTREAM_API
#define SLIPSTREAM_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* A descriptor that becomes readable when one of these signals arrives.
 * Answers the descriptor, or a NEGATED errno - liburing's convention,
 * which the rest of this library already follows.
 *
 * On Windows the numbers are still SIGINT and SIGTERM. The console gives
 * CTRL_C_EVENT and CTRL_CLOSE_EVENT, and this maps them.
 *
 * One open per process. A second call refuses with -EEXIST: the console
 * takes one handler, and two producers on one wire cannot say who wrote. */
SLIPSTREAM_API int slipstream_signal_open(const int *signums, unsigned n);

/* Take one arrival. Answers 1 and writes the signal number, 0 when
 * nothing waits, or a negated errno. The descriptor is non-blocking, so
 * 0 is an answer and not a wait. */
SLIPSTREAM_API int slipstream_signal_read(int fd, int *signum);

/* Give it back. Answers 0, or a negated errno. */
SLIPSTREAM_API int slipstream_signal_close(int fd);

#ifdef __cplusplus
}
#endif

#endif /* SLIPSTREAM_SIGNAL_H */
