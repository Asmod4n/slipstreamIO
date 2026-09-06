/* One scenario, read twice: once from the arm this binary was built
 * with, and compared against what inotify itself says. The Makefile
 * builds this file twice - test/inotify on the kernel's inotify, and
 * test/inotify_posix with SLIPSTREAM_INOTIFY_NO_INOTIFY - so the same
 * expectations run against both arms, the way test/signal does.
 *
 * The oracle is inotify(7), written out here as what each operation
 * must produce. A caller cannot see which arm answered, so neither can
 * this test: it makes the same files in the same order and checks the
 * same records.
 */
#define _GNU_SOURCE 1

#include "slipstream_inotify.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int fails;

static void ok(int cond, const char *what) {
  printf("%s %s\n", cond ? "ok  " : "FAIL", what);
  if (!cond) fails++;
}

/* Every record that arrived within the deadline, in order. A record
 * from an arm that looks arrives late, so the reader waits for the
 * events rather than for a moment. */
struct got {
  int wd;
  uint32_t mask;
  uint32_t cookie;
  char name[256];
};

static unsigned drain(int fd, struct got *out, unsigned max, unsigned want, int ms) {
  unsigned n = 0;
  const int step = 20;
  int waited = 0;
  while (n < want && waited < ms) {
    char buf[8192];
    int got;
    struct pollfd p;
    p.fd = fd;
    p.events = POLLIN;
    if (poll(&p, 1, step) <= 0) {
      waited += step;
      continue;
    }
    got = slipstream_inotify_read(fd, buf, (unsigned) sizeof(buf));
    if (got <= 0) {
      waited += step;
      continue;
    }
    for (int at = 0; at + (int) sizeof(struct slipstream_inotify_event) <= got;) {
      struct slipstream_inotify_event ev;
      memcpy(&ev, buf + at, sizeof(ev));
      at += (int) sizeof(ev);
      if (n < max) {
        out[n].wd = ev.wd;
        out[n].mask = ev.mask;
        out[n].cookie = ev.cookie;
        out[n].name[0] = '\0';
        if (ev.len != 0 && at + (int) ev.len <= got) {
          snprintf(out[n].name, sizeof(out[n].name), "%s", buf + at);
        }
        n++;
      }
      at += (int) ev.len;
    }
  }
  return n;
}

/* One record with this mask and this name, whatever else arrived beside
 * it. Both arms may add records the other does not - inotify sends
 * IN_OPEN and IN_CLOSE_WRITE for a write, which nothing else can see -
 * so the test asks whether what MUST be there is there. */
static const struct got *find(const struct got *g, unsigned n, uint32_t mask,
                              const char *name) {
  for (unsigned i = 0; i < n; i++) {
    if ((g[i].mask & mask) == 0) continue;
    if (name == NULL) return &g[i];
    if (strcmp(g[i].name, name) == 0) return &g[i];
  }
  return NULL;
}

static void put(const char *path, const char *text) {
  FILE *f = fopen(path, "wb");
  if (f == NULL) return;
  fwrite(text, 1, strlen(text), f);
  fclose(f);
}

int main(void) {
  char dir[] = "/tmp/slipstream-inotify-XXXXXX";
  char a[512];
  char b[512];
  struct got g[64];
  unsigned n;
  int fd;
  int wd;

  if (mkdtemp(dir) == NULL) {
    perror("mkdtemp");
    return 1;
  }
  snprintf(a, sizeof(a), "%s/one.txt", dir);
  snprintf(b, sizeof(b), "%s/two.txt", dir);

  fd = slipstream_inotify_init1(SLIPSTREAM_IN_NONBLOCK | SLIPSTREAM_IN_CLOEXEC);
  ok(fd >= 0, "init1 answers a descriptor");
  if (fd < 0) return 1;

  ok(slipstream_inotify_add_watch(fd, "/nowhere/at/all", SLIPSTREAM_IN_ALL_EVENTS) == -ENOENT,
     "a path that is not there answers -ENOENT");
  ok(slipstream_inotify_add_watch(fd, dir, 0) == -EINVAL, "an empty mask answers -EINVAL");

  wd = slipstream_inotify_add_watch(fd, dir, SLIPSTREAM_IN_ALL_EVENTS);
  ok(wd > 0, "add_watch answers a watch descriptor");
  ok(slipstream_inotify_add_watch(fd, dir, SLIPSTREAM_IN_ALL_EVENTS) == wd,
     "the same path answers the same wd");

  /* A file that appears. */
  put(a, "first");
  n = drain(fd, g, 64, 1, 2000);
  ok(find(g, n, SLIPSTREAM_IN_CREATE, "one.txt") != NULL, "a new file is IN_CREATE with its name");
  ok(find(g, n, SLIPSTREAM_IN_CREATE, "one.txt") == NULL ||
         find(g, n, SLIPSTREAM_IN_CREATE, "one.txt")->wd == wd,
     "the record carries the wd of the watch");

  /* A file that changes. */
  put(a, "first, and more");
  n = drain(fd, g, 64, 1, 2000);
  ok(find(g, n, SLIPSTREAM_IN_MODIFY, "one.txt") != NULL, "a written file is IN_MODIFY");

  /* A name that moves inside the watched directory. */
  ok(rename(a, b) == 0, "rename inside the directory");
  n = drain(fd, g, 64, 2, 2000);
  {
    const struct got *from = find(g, n, SLIPSTREAM_IN_MOVED_FROM, "one.txt");
    const struct got *to = find(g, n, SLIPSTREAM_IN_MOVED_TO, "two.txt");
    ok(from != NULL, "the name it left is IN_MOVED_FROM");
    ok(to != NULL, "the name it took is IN_MOVED_TO");
    ok(from != NULL && to != NULL && from->cookie == to->cookie && from->cookie != 0,
       "one cookie pairs the two halves");
  }

  /* A directory inside the watched one, so ISDIR is asked for. */
  snprintf(a, sizeof(a), "%s/sub", dir);
  ok(mkdir(a, 0755) == 0, "a directory inside it");
  n = drain(fd, g, 64, 1, 2000);
  {
    const struct got *made = find(g, n, SLIPSTREAM_IN_CREATE, "sub");
    ok(made != NULL, "a new directory is IN_CREATE with its name");
    ok(made != NULL && (made->mask & SLIPSTREAM_IN_ISDIR) != 0, "and it carries IN_ISDIR");
  }

  /* A file that goes. */
  ok(unlink(b) == 0, "unlink");
  n = drain(fd, g, 64, 1, 2000);
  ok(find(g, n, SLIPSTREAM_IN_DELETE, "two.txt") != NULL, "a removed file is IN_DELETE");

  /* The watch, given back. */
  ok(slipstream_inotify_rm_watch(fd, wd) == 0, "rm_watch answers 0");
  n = drain(fd, g, 64, 1, 2000);
  ok(find(g, n, SLIPSTREAM_IN_IGNORED, NULL) != NULL, "and the watch says IN_IGNORED");
  ok(slipstream_inotify_rm_watch(fd, wd) == -EINVAL, "a wd that is gone answers -EINVAL");

  /* The watched thing itself, removed under the watch. */
  wd = slipstream_inotify_add_watch(fd, a, SLIPSTREAM_IN_ALL_EVENTS);
  ok(wd > 0, "watch the directory inside");
  ok(rmdir(a) == 0, "rmdir it");
  n = drain(fd, g, 64, 2, 3000);
  ok(find(g, n, SLIPSTREAM_IN_DELETE_SELF, NULL) != NULL,
     "the watched thing going is IN_DELETE_SELF");
  ok(find(g, n, SLIPSTREAM_IN_IGNORED, NULL) != NULL, "and then IN_IGNORED");

  ok(slipstream_inotify_close(fd) == 0, "close answers 0");
  ok(slipstream_inotify_rm_watch(fd, 1) == -EBADF, "a descriptor that is closed answers -EBADF");

  rmdir(dir);
  printf(fails == 0 ? "inotify: all ok\n" : "inotify: %d failed\n", fails);
  return fails == 0 ? 0 : 1;
}
