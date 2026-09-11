/* See slipstream_tmpfile.h for what this is and why it is not
 * memfd_create(2). */

/* O_TMPFILE and AT_SYMLINK_FOLLOW are the two the POSIX arm needs
 * beyond the base, and glibc hides both behind _GNU_SOURCE. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "slipstream_tmpfile.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

#if defined(_WIN32)

#include <windows.h>
#include <io.h>
#include <fcntl.h>

/* Windows cannot unlink an open file, so it names the moment instead:
 * FILE_FLAG_DELETE_ON_CLOSE removes the entry when the last handle
 * goes. FILE_ATTRIBUTE_TEMPORARY asks the cache manager to keep the
 * pages and not write them out unless it must.
 *
 * GetTempFileNameW makes the file as well as the name, so the name is
 * never free for somebody else between the two calls. It is opened
 * again here to get the flags, and the first handle is closed. */
SLIPSTREAM_API int slipstream_tmpfile(const char *dir) {
  WCHAR dirw[MAX_PATH];
  WCHAR name[MAX_PATH];

  if (dir == NULL) {
    const DWORD n = GetTempPathW(MAX_PATH, dirw);
    if (n == 0 || n > MAX_PATH) return -ENOENT;
  } else {
    const int n = MultiByteToWideChar(CP_UTF8, 0, dir, -1, dirw, MAX_PATH);
    if (n == 0) return -ENAMETOOLONG;
  }

  if (GetTempFileNameW(dirw, L"sls", 0, name) == 0) return -EACCES;

  const HANDLE h = CreateFileW(
      name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
      FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
  if (h == INVALID_HANDLE_VALUE) {
    DeleteFileW(name);
    return -EACCES;
  }

  const int fd = _open_osfhandle((intptr_t)h, _O_BINARY | _O_RDWR);
  if (fd < 0) {
    CloseHandle(h);
    return -EMFILE;
  }
  return fd;
}

#else

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define SLIP_TMPFILE_LEAF "/slipstream-XXXXXX"

/* Linux 3.11: a file with no name at all, made in a directory rather
 * than under one. Nothing is ever created in the directory, so there is
 * no entry to collide with, no entry to unlink, and no window in which
 * another process could open it.
 *
 * It is also the only form this library can link into place later: a
 * name can be given to such a file once, and a file that was unlinked
 * can never get one back. See slipstream_tmpfile_link.
 *
 * Not every filesystem implements it. The caller never learns which
 * arm ran, because both answer a descriptor that behaves the same -
 * except for the link, which says so itself.
 */
#if defined(__linux__) && defined(O_TMPFILE)
static int slip_tmpfile_anon(const char *dir) {
  const int fd = open(dir, O_TMPFILE | O_RDWR | O_CLOEXEC, S_IRUSR | S_IWUSR);
  if (fd < 0) return -errno;
  return fd;
}
#else
static int slip_tmpfile_anon(const char *dir) {
  (void)dir;
  return -EOPNOTSUPP;
}
#endif

SLIPSTREAM_API int slipstream_tmpfile(const char *dir) {
  char path[4096];

  if (dir == NULL) dir = getenv("TMPDIR");
  if (dir == NULL || dir[0] == '\0') dir = "/tmp";

  {
    const int anon = slip_tmpfile_anon(dir);
    if (anon >= 0) return anon;
    /* Every other failure is the directory's, and mkstemp would meet
     * it as well: no room, no permission, no such directory. Only a
     * filesystem that does not implement O_TMPFILE falls through. */
    if (anon != -EOPNOTSUPP && anon != -EISDIR && anon != -EINVAL && anon != -ENOSYS) {
      return anon;
    }
  }

  const size_t dlen = strlen(dir);
  /* One byte for the NUL, and the leaf carries its own leading slash. A
   * directory that ends in one would give a double slash, which every
   * POSIX path resolver folds, so it is not worth a branch. */
  if (dlen + sizeof(SLIP_TMPFILE_LEAF) > sizeof(path)) return -ENAMETOOLONG;
  memcpy(path, dir, dlen);
  memcpy(path + dlen, SLIP_TMPFILE_LEAF, sizeof(SLIP_TMPFILE_LEAF));

  const int fd = mkstemp(path);
  if (fd < 0) return -errno;

  /* The mode is said here rather than left to the umask. POSIX.1-2008
   * asks mkstemp for 0600, and glibc 2.06 and older made the file 0666.
   * A caller cannot know which one it stands on, and it must not have to
   * set a process-wide umask to find out. */
  if (fchmod(fd, S_IRUSR | S_IWUSR) < 0) goto fail;

  /* mkostemp(3) would carry O_CLOEXEC into the open. It is GNU, so this
   * is the portable half of the same thing: the window between the two
   * calls is this thread's own, and no exec happens inside it. */
  {
    const int flags = fcntl(fd, F_GETFD);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) goto fail;
  }

  /* The name goes, the file stays: the descriptor holds the last
   * reference, so the blocks go back when it closes, and no other
   * process can open what it cannot name. */
  if (unlink(path) < 0) goto fail;

  return fd;

fail:
  {
    const int e = errno;
    unlink(path);
    close(fd);
    return -e;
  }
}

/* A name for a file that has none. Linux only, and only for a
 * descriptor slipstream_tmpfile made with O_TMPFILE: a file that was
 * unlinked cannot be linked again, and the kernel says so with ENOENT.
 *
 * linkat with AT_EMPTY_PATH is the direct form and it needs
 * CAP_DAC_READ_SEARCH, which a server does not have. /proc/self/fd/N
 * is the form that works without it, and it is what the kernel's own
 * documentation names for this.
 *
 * The link fails with EEXIST rather than replacing anything: an upload
 * that lands on top of a file nobody asked it to replace is a defect,
 * not a feature. A caller that wants to replace links to a name of its
 * own and renames.
 */
SLIPSTREAM_API int slipstream_tmpfile_link(int fd, const char *path) {
  char proc[64];

  if (fd < 0 || path == NULL || path[0] == '\0') return -EINVAL;
#if defined(__linux__) && defined(AT_SYMLINK_FOLLOW)
  snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
  if (linkat(AT_FDCWD, proc, AT_FDCWD, path, AT_SYMLINK_FOLLOW) < 0) return -errno;
  return 0;
#else
  (void)proc;
  return -EOPNOTSUPP;
#endif
}

#endif
