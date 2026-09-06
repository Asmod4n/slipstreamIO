/* Watching files, as something the engine can wait for.
 *
 * A program that serves files wants to hear that one changed. Linux
 * turns that into a descriptor - inotify - and the loop reads it like
 * any other. No other platform has inotify.
 *
 * This gives every platform that descriptor, and it gives it inotify's
 * own shape: the same three calls, the same mask bits, the same record
 * on the wire. A caller writes one thing:
 *
 *   int fd = slipstream_inotify_init1(SLIPSTREAM_IN_NONBLOCK);
 *   int wd = slipstream_inotify_add_watch(fd, "/srv/site", SLIPSTREAM_IN_ALL_EVENTS);
 *   io_uring_prep_poll_add(sqe, fd, POLLIN);
 *
 * and reads packed slipstream_inotify_event records out of the fd, the
 * way inotify(7) describes.
 *
 * BEHAVIOUR IS THE CONTRACT, NOT THE CALL SHAPE. A caller cannot see
 * which arm answered, and must not have to: the same sequence of file
 * system operations produces the same sequence of records, field for
 * field - wd, mask, cookie, name and its padding, and the order the
 * records arrive in. inotify is the oracle, and test/inotify.c proves
 * it the way test/parity.c proves the engine: it runs one scenario
 * twice on Linux, once on the kernel's inotify and once on the arm
 * below, and the two streams must agree.
 *
 * That covers what a build cares about: CREATE, MODIFY, DELETE,
 * MOVED_FROM and MOVED_TO with one cookie between them, ATTRIB,
 * DELETE_SELF, MOVE_SELF, IGNORED, and ISDIR on the ones about a
 * directory. It covers the descriptor as well: whole records only, a
 * buffer too small for one record answers EINVAL, an empty read on a
 * NONBLOCK descriptor answers EAGAIN, and a wd is an increasing number
 * per descriptor that a second add of the same path answers again.
 *
 * WHAT NO ARM CAN MAKE THE SAME IS TIME. inotify hears a change as the
 * kernel makes it. Where there is no inotify, the arm below hears it by
 * looking again, so:
 *
 *   - a record arrives late, by up to one look;
 *   - two changes to one file between two looks are one record, the
 *     way inotify also coalesces two IN_MODIFY nobody read yet;
 *   - a file created and deleted between two looks left nothing to see,
 *     and is not reported at all.
 *
 * And four events have no file system trace, so only Linux can send
 * them: IN_ACCESS, IN_OPEN, IN_CLOSE_WRITE and IN_CLOSE_NOWRITE. A
 * caller that asks for them off Linux gets the watch, and never those
 * bits. Everything a build does is a change on disk, so nothing that
 * matters here rides on them.
 *
 * A watch is not recursive, on any platform. inotify is not either:
 * a caller that wants a tree adds one watch per directory.
 */
#ifndef SLIPSTREAM_INOTIFY_H
#define SLIPSTREAM_INOTIFY_H

#include <stdint.h>

#ifndef SLIPSTREAM_API
#define SLIPSTREAM_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* inotify(7)'s record, field for field. name is NUL terminated and then
 * padded with NULs, so len is a multiple of 16 and the next record
 * begins at a known offset. len is 0 when there is no name, which is
 * every event about the watched thing itself. */
struct slipstream_inotify_event {
  int32_t wd;
  uint32_t mask;
  uint32_t cookie;
  uint32_t len;
  /* char name[]; follows, len bytes, when len is not 0. */
};

/* The mask bits, with inotify's own values. */
#define SLIPSTREAM_IN_ACCESS 0x00000001u
#define SLIPSTREAM_IN_MODIFY 0x00000002u
#define SLIPSTREAM_IN_ATTRIB 0x00000004u
#define SLIPSTREAM_IN_CLOSE_WRITE 0x00000008u
#define SLIPSTREAM_IN_CLOSE_NOWRITE 0x00000010u
#define SLIPSTREAM_IN_OPEN 0x00000020u
#define SLIPSTREAM_IN_MOVED_FROM 0x00000040u
#define SLIPSTREAM_IN_MOVED_TO 0x00000080u
#define SLIPSTREAM_IN_CREATE 0x00000100u
#define SLIPSTREAM_IN_DELETE 0x00000200u
#define SLIPSTREAM_IN_DELETE_SELF 0x00000400u
#define SLIPSTREAM_IN_MOVE_SELF 0x00000800u

/* Sent by the watch itself, never asked for. */
#define SLIPSTREAM_IN_UNMOUNT 0x00002000u
#define SLIPSTREAM_IN_Q_OVERFLOW 0x00004000u
#define SLIPSTREAM_IN_IGNORED 0x00008000u

/* Said about an event, not asked for. */
#define SLIPSTREAM_IN_ISDIR 0x40000000u

#define SLIPSTREAM_IN_CLOSE (SLIPSTREAM_IN_CLOSE_WRITE | SLIPSTREAM_IN_CLOSE_NOWRITE)
#define SLIPSTREAM_IN_MOVE (SLIPSTREAM_IN_MOVED_FROM | SLIPSTREAM_IN_MOVED_TO)
#define SLIPSTREAM_IN_ALL_EVENTS 0x00000fffu

/* Flags for init1, with inotify's own values. */
#define SLIPSTREAM_IN_CLOEXEC 0x00080000
#define SLIPSTREAM_IN_NONBLOCK 0x00000800

/* A descriptor that becomes readable when a watched thing changes.
 * Answers the descriptor, or a NEGATED errno - liburing's convention,
 * which the rest of this library already follows.
 *
 * flags takes SLIPSTREAM_IN_CLOEXEC and SLIPSTREAM_IN_NONBLOCK, and
 * nothing else. A loop that polls the descriptor wants NONBLOCK, so
 * that an empty read answers instead of waiting. */
SLIPSTREAM_API int slipstream_inotify_init1(int flags);

/* Watch one path. Answers a watch descriptor, which is what the wd
 * field of every event about that path carries, or a negated errno.
 *
 * A path already watched on this descriptor answers the SAME wd and
 * replaces the mask, which is what inotify does. */
SLIPSTREAM_API int slipstream_inotify_add_watch(int fd, const char *path, uint32_t mask);

/* Stop watching. Answers 0, or a negated errno. One last event with
 * SLIPSTREAM_IN_IGNORED arrives for that wd, as inotify sends. */
SLIPSTREAM_API int slipstream_inotify_rm_watch(int fd, int wd);

/* Give the descriptor back. Answers 0, or a negated errno. Everything
 * watched through it is forgotten. */
SLIPSTREAM_API int slipstream_inotify_close(int fd);

#ifdef __cplusplus
}
#endif

#endif /* SLIPSTREAM_INOTIFY_H */
