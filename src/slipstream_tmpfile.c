/* See slipstream_tmpfile.h for what this is and why it is not
 * memfd_create(2). */

/* The Makefile builds with -std=c11 and no feature macro, which is a
 * rule about the exported headers: they have to stand up for a C
 * consumer on their own terms. A translation unit of ours is not a
 * header, and under a strict -std= glibc declares neither mkstemp nor
 * fchmod. Both were called here with no declaration in scope, which the
 * compiler accepted with a warning and C23 refuses outright. The macro
 * is asked for here, where it changes nothing anybody else sees.
 *
 * _GNU_SOURCE rather than _POSIX_C_SOURCE, because it also declares
 * mkostemp, which is what closes the descriptor-flag window below. */
#ifndef _WIN32
#define _GNU_SOURCE
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
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

/* glibc, musl and the BSDs carry mkostemp; macOS does not. */
#if defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#define SLIP_HAVE_MKOSTEMP 1
#endif

#define SLIP_TMPFILE_LEAF "/slipstream-XXXXXX"

SLIPSTREAM_API int slipstream_tmpfile(const char *dir) {
  char path[4096];

  if (dir == NULL) dir = getenv("TMPDIR");
  if (dir == NULL || dir[0] == '\0') dir = "/tmp";

  const size_t dlen = strlen(dir);
  /* One byte for the NUL, and the leaf carries its own leading slash. A
   * directory that ends in one would give a double slash, which every
   * POSIX path resolver folds, so it is not worth a branch. */
  if (dlen + sizeof(SLIP_TMPFILE_LEAF) > sizeof(path)) return -ENAMETOOLONG;
  memcpy(path, dir, dlen);
  memcpy(path + dlen, SLIP_TMPFILE_LEAF, sizeof(SLIP_TMPFILE_LEAF));

  /* mkostemp carries O_CLOEXEC into the open itself. That matters for a
   * caller with more than one thread: between a plain mkstemp and a
   * later F_SETFD, any other thread may fork and exec, and the child
   * then inherits a descriptor to this file. The comment that used to
   * stand here said the window was "this thread's own", which is a
   * claim about the caller and not true of a threaded one.
   *
   * Where mkostemp is absent - macOS is the one that matters - the old
   * two-step stands, with the window named rather than denied. */
#ifdef SLIP_HAVE_MKOSTEMP
  const int fd = mkostemp(path, O_CLOEXEC);
#else
  const int fd = mkstemp(path);
#endif
  if (fd < 0) return -errno;

  /* The mode is said here rather than left to the umask. POSIX.1-2008
   * asks mkstemp for 0600, and glibc 2.06 and older made the file 0666.
   * A caller cannot know which one it stands on, and it must not have to
   * set a process-wide umask to find out. */
  if (fchmod(fd, S_IRUSR | S_IWUSR) < 0) goto fail;

#ifndef SLIP_HAVE_MKOSTEMP
  /* The fallback, and the window is real: another thread that execs
   * between the open above and this line hands the child a descriptor
   * to this file. Nothing portable closes it without mkostemp. */
  {
    const int flags = fcntl(fd, F_GETFD);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) goto fail;
  }
#endif

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

#endif
