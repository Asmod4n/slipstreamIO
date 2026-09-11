/* A temporary file, as a descriptor and nothing else.
 *
 * A server that takes an upload cannot hold it in memory: one request
 * decides how much, and the client decides how many. The bytes go to a
 * file, and the file is nobody's business but this process's - it has no
 * name to open, no name to collide with, and no name to leave behind
 * when the process dies.
 *
 *   int fd = slipstream_tmpfile(NULL);
 *   io_uring_prep_write(sqe, fd, buf, len, off);
 *
 * On Linux that is O_TMPFILE: the file is made in a directory and never
 * appears in it, so there is no entry to collide with, no entry to
 * unlink, and no window in which another process could open it. It is
 * also the only form that can be given a name afterwards - see
 * slipstream_tmpfile_link.
 *
 * Where the filesystem does not implement O_TMPFILE, and on every other
 * POSIX platform, it is mkstemp(3) and then unlink(2), which is the
 * oldest idiom there is: the directory entry goes, the open descriptor
 * keeps the file, and the last close frees the blocks. mkostemp(3)
 * would do both steps in one, and it is GNU only, so this does not use
 * it.
 *
 * Not memfd_create(2), which has no name at all: a memfd is tmpfs, so
 * the bytes are memory, and a server that spills to memory to stop
 * running out of memory has not spilled.
 *
 * Windows cannot unlink an open file, so the moment differs there and a
 * caller must know it. FILE_FLAG_DELETE_ON_CLOSE removes the entry when
 * the last handle goes, so the name is in the directory until then. No
 * other process can open it while it is there - the share mode is 0 -
 * and nothing is left in the directory after the descriptor closes,
 * which is what both arms promise.
 */
#ifndef SLIPSTREAM_TMPFILE_H
#define SLIPSTREAM_TMPFILE_H

#ifndef SLIPSTREAM_API
#define SLIPSTREAM_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* A file to write, already gone from its directory. Answers the
 * descriptor, or a NEGATED errno - liburing's convention, which the rest
 * of this library already follows.
 *
 * `dir` names where it is made. NULL asks the platform: TMPDIR and then
 * /tmp on POSIX, GetTempPathW on Windows. Name one to put the bytes on a
 * disk that has room for them - the default is small on many machines,
 * and on some it is tmpfs, which is memory.
 *
 * The mode is 0600, set on the file and not left to the umask. The
 * descriptor is close-on-exec.
 */
SLIPSTREAM_API int slipstream_tmpfile(const char *dir);

/* A name for the file behind `fd`, in the filesystem. 0, or a NEGATED
 * errno.
 *
 * This is what O_TMPFILE buys, and it is why this library prefers it:
 * a file that never had a name can be given one, so an upload that was
 * written to a temporary file becomes the file it was meant to be
 * without a copy. A file that was made by the mkstemp arm was unlinked
 * at birth and can never be linked again - the kernel answers -ENOENT,
 * and that is the honest answer rather than a silent copy.
 *
 * -EEXIST when `path` is taken: this never replaces. A caller that
 * wants to replace links to a name of its own and renames over the
 * target, which is the only form that is atomic.
 *
 * -EOPNOTSUPP where the platform has no such call, which is everything
 * that is not Linux, Windows included.
 *
 * The directory of `path` and the directory the file was made in have
 * to be the same filesystem, because a link never crosses one.
 */
SLIPSTREAM_API int slipstream_tmpfile_link(int fd, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* SLIPSTREAM_TMPFILE_H */
