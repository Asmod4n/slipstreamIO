/* A temporary file, as a descriptor. One scene per arm: make it, write
 * it, read it back, and see what its directory holds.
 *
 * The two arms do not lose the name at the same moment, and this counts
 * the difference rather than hiding it. POSIX unlinks at once, so the
 * entry is gone while the file is still open. Windows cannot unlink an
 * open file: FILE_FLAG_DELETE_ON_CLOSE removes the entry when the last
 * handle goes, so the name is there until then - and no other process
 * can open it, because the share mode is 0.
 *
 * What both promise is the part a caller needs: nothing is left in the
 * directory after the descriptor closes, on either arm.
 *
 * The POSIX arm runs here. The Windows arm is this same source under
 * Wine (test/tmpfile_wine.sh).
 */
#define _GNU_SOURCE 1
#define _DEFAULT_SOURCE 1

#include "slipstream_tmpfile.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#define slip_read _read
#define slip_write _write
#define slip_lseek _lseek
#define slip_close _close
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#define slip_read read
#define slip_write write
#define slip_lseek lseek
#define slip_close close
#endif

static int failed;

static void ok(int cond, const char *what) {
  printf("%s %s\n", cond ? "ok  " : "FAIL", what);
  if (!cond) failed = 1;
}

/* How many entries the directory holds, counting neither "." nor "..". */
static int entries_in(const char *dir) {
#ifdef _WIN32
  char pattern[MAX_PATH];
  snprintf(pattern, sizeof(pattern), "%s\\*", dir);
  WIN32_FIND_DATAA e;
  const HANDLE h = FindFirstFileA(pattern, &e);
  if (h == INVALID_HANDLE_VALUE) return -1;
  int n = 0;
  do {
    if (strcmp(e.cFileName, ".") != 0 && strcmp(e.cFileName, "..") != 0) n++;
  } while (FindNextFileA(h, &e));
  FindClose(h);
  return n;
#else
  DIR *d = opendir(dir);
  if (d == NULL) return -1;
  int n = 0;
  const struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0) n++;
  }
  closedir(d);
  return n;
#endif
}

int main(void) {
  /* A directory of this test's own, so the count below is only about
   * what this test makes. */
#ifdef _WIN32
  char dir[MAX_PATH];
  GetTempPathA(MAX_PATH, dir);
  strcat(dir, "slipstream-tmpfile-test");
  CreateDirectoryA(dir, NULL);
#else
  char dir[] = "/tmp/slipstream-tmpfile-XXXXXX";
  if (mkdtemp(dir) == NULL) {
    printf("FAIL cannot make a directory to test in: %s\n", strerror(errno));
    return 1;
  }
#endif

  const int fd = slipstream_tmpfile(dir);
  ok(fd >= 0, "slipstream_tmpfile answers a descriptor");
  if (fd < 0) {
    printf("     it answered %d (%s)\n", fd, strerror(-fd));
    return 1;
  }

  /* It is a file that takes bytes and gives them back. */
  const char want[] = "a body too large to keep in memory";
  ok(slip_write(fd, want, sizeof(want)) == (int)sizeof(want), "it takes bytes");
  ok(slip_lseek(fd, 0, SEEK_SET) == 0, "it seeks to the start");
  char got[sizeof(want)];
  memset(got, 0, sizeof(got));
  ok(slip_read(fd, got, sizeof(got)) == (int)sizeof(want), "it gives them back");
  ok(memcmp(want, got, sizeof(want)) == 0, "the bytes are the ones written");

  /* What the directory holds while the file is open. The two arms differ
   * here, and each says which one it is. */
#ifdef _WIN32
  ok(entries_in(dir) == 1, "the name is there while it is open (delete on close)");
#else
  ok(entries_in(dir) == 0, "the name is gone while it is open (unlink)");
#endif

  ok(slip_close(fd) == 0, "it gives back");

  /* And this is what both promise: nothing left behind. */
  ok(entries_in(dir) == 0, "the directory is empty once it closes");

#if !defined(_WIN32) && defined(__linux__)
  /* The name a file with no name can be given. This is what O_TMPFILE
   * buys: an upload becomes the file it was meant to be, with no copy.
   * A filesystem without O_TMPFILE takes the mkstemp arm, whose file
   * was unlinked at birth and can never be linked - the kernel answers
   * ENOENT, and the scene says which arm it stood on rather than
   * failing. */
  {
    char target[512];
    const int lf = slipstream_tmpfile(dir);
    ok(lf >= 0, "a second descriptor, to link into place");
    if (lf >= 0) {
      const char body[] = "an upload that becomes a file";
      ok(slip_write(lf, body, sizeof(body)) == (int)sizeof(body), "it takes the bytes");
      snprintf(target, sizeof(target), "%s/landed", dir);
      const int rc = slipstream_tmpfile_link(lf, target);
      if (rc == 0) {
        ok(entries_in(dir) == 1, "the link put it in the directory");
        struct stat st;
        ok(stat(target, &st) == 0 && (size_t)st.st_size == sizeof(body),
           "the file at the name holds the bytes");
        /* Twice is EEXIST: this never replaces. */
        ok(slipstream_tmpfile_link(lf, target) == -EEXIST, "a taken name is refused");
        unlink(target);
      } else {
        ok(rc == -ENOENT, "the mkstemp arm cannot link, and says ENOENT");
      }
      slip_close(lf);
    }
    ok(entries_in(dir) == 0, "the directory is empty again");
  }
#endif

#ifdef _WIN32
  RemoveDirectoryA(dir);
#else
  rmdir(dir);
#endif

  printf(failed ? "tmpfile: FAILED\n" : "tmpfile: every scene\n");
  return failed;
}
