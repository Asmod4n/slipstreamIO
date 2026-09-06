/* slipstream_inotify.h, three times. See that header for the contract,
 * and for the one rule that decides everything here: a caller cannot
 * see which arm answered.
 *
 * Linux hands the work to the kernel. The other two make the same
 * records themselves and put them on a socket pair, because that is
 * what a caller can poll.
 */
/* inotify, socketpair and the dirent calls sit behind these under a
 * strict -std=c11, which is what this Makefile builds with. Declared on
 * the first lines, as engine_posix.c does. */
#define _GNU_SOURCE 1
#define _DEFAULT_SOURCE 1

#include "slipstream_inotify.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Linux: the kernel already does it -------------------------------- */
/* SLIPSTREAM_INOTIFY_NO_INOTIFY takes this arm out of the build, so the
 * generic arm below can be compiled and proven on a Linux host - and,
 * with both in one process, compared against this one event for event.
 * The same reason slipstream_signal has SLIPSTREAM_SIGNAL_NO_SIGNALFD:
 * an arm nobody can run is an arm nobody has checked. */
#if defined(__linux__) && !defined(SLIPSTREAM_INOTIFY_NO_INOTIFY)

#include <sys/inotify.h>
#include <unistd.h>

int slipstream_inotify_init1(int flags) {
  int f = 0;
  if ((flags & ~(SLIPSTREAM_IN_CLOEXEC | SLIPSTREAM_IN_NONBLOCK)) != 0) return -EINVAL;
  if (flags & SLIPSTREAM_IN_CLOEXEC) f |= IN_CLOEXEC;
  if (flags & SLIPSTREAM_IN_NONBLOCK) f |= IN_NONBLOCK;
  {
    const int fd = inotify_init1(f);
    return fd < 0 ? -errno : fd;
  }
}

int slipstream_inotify_add_watch(int fd, const char *path, uint32_t mask) {
  int wd;
  if (path == NULL) return -EFAULT;
  wd = inotify_add_watch(fd, path, mask);
  return wd < 0 ? -errno : wd;
}

int slipstream_inotify_rm_watch(int fd, int wd) {
  return inotify_rm_watch(fd, wd) == 0 ? 0 : -errno;
}

int slipstream_inotify_close(int fd) { return close(fd) == 0 ? 0 : -errno; }

/* ---- Every other POSIX: look, look again, report the difference ------- */
#elif !defined(_WIN32)

#include <dirent.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "thrd_compat.h"

/* How long between two looks. The header says a record arrives late by
 * up to one look, so this is the number that says how late. 100 ms is
 * under what a person notices in a browser reload and far above what a
 * scan of a site directory costs. */
#ifndef SLIPSTREAM_INOTIFY_INTERVAL_MS
#define SLIPSTREAM_INOTIFY_INTERVAL_MS 100
#endif

/* One name in a watched directory, as the last look found it. The three
 * numbers are what a change shows up in: st_ino tells a replaced file
 * from a written one, and mtime with size tells a written file from an
 * untouched one. */
struct entry {
  char *name;
  uint64_t ino;
  int64_t mtime_sec;
  long mtime_nsec;
  uint64_t size;
  uint32_t mode;
  int seen;
};

struct watch {
  int wd;
  char *path;
  uint32_t mask;
  int is_dir;
  int dead;
  /* The watched thing itself, so DELETE_SELF, MOVE_SELF and ATTRIB on
   * it are answered from the same look. */
  uint64_t self_ino;
  int64_t self_mtime_sec;
  long self_mtime_nsec;
  uint64_t self_size;
  uint32_t self_mode;
  struct entry *kids;
  size_t nkids;
};

struct inst {
  int rd;
  int wr;
  int used;
  int stopping;
  int next_wd;
  uint32_t next_cookie;
  int overflowed;
  mtx_t lock;
  thrd_t thread;
  struct watch *watches;
  size_t nwatches;
};

/* A caller holds the read end and nothing else, so the read end is the
 * key. A descriptor is small and dense, and the table is the array
 * indexed by it - the same shape engine_posix.c keeps its parked set in. */
#define SLIPSTREAM_INOTIFY_MAX 64
static struct inst g_inst[SLIPSTREAM_INOTIFY_MAX];
static mtx_t g_lock;
static once_flag g_once = ONCE_FLAG_INIT;

static void inotify_once(void) { mtx_init(&g_lock, mtx_plain); }

static struct inst *inst_of(int fd) {
  for (unsigned i = 0; i < SLIPSTREAM_INOTIFY_MAX; i++) {
    if (g_inst[i].used && g_inst[i].rd == fd) return &g_inst[i];
  }
  return NULL;
}

/* inotify pads name with NULs so len is a multiple of 16 and the next
 * record starts where a reader can find it. */
static uint32_t name_len(const char *name) {
  size_t n;
  if (name == NULL) return 0;
  n = strlen(name) + 1;
  return (uint32_t) ((n + 15) & ~(size_t) 15);
}

/* One record, whole. A short write would leave half a record on the
 * wire, and a reader has no way to find the next one, so a write that
 * cannot go whole becomes an overflow instead - which is exactly what
 * inotify does when its own queue is full. */
static void emit(struct inst *in, int wd, uint32_t mask, uint32_t cookie, const char *name) {
  char buf[sizeof(struct slipstream_inotify_event) + 256];
  struct slipstream_inotify_event ev;
  const uint32_t len = name_len(name);
  size_t total;
  ssize_t put;

  if (len > 256) return;
  ev.wd = (int32_t) wd;
  ev.mask = mask;
  ev.cookie = cookie;
  ev.len = len;
  memcpy(buf, &ev, sizeof(ev));
  if (len != 0) {
    memset(buf + sizeof(ev), 0, len);
    memcpy(buf + sizeof(ev), name, strlen(name));
  }
  total = sizeof(ev) + len;
  put = send(in->wr, buf, total, MSG_NOSIGNAL);
  if (put == (ssize_t) total) {
    in->overflowed = 0;
    return;
  }
  /* inotify sends ONE overflow record for a run of lost events, with
   * wd -1 and no name, and sends it again only after one gets through. */
  if (!in->overflowed) {
    struct slipstream_inotify_event over;
    over.wd = -1;
    over.mask = SLIPSTREAM_IN_Q_OVERFLOW;
    over.cookie = 0;
    over.len = 0;
    if (send(in->wr, &over, sizeof(over), MSG_NOSIGNAL) == (ssize_t) sizeof(over)) {
      in->overflowed = 1;
    }
  }
}

/* What a watch asked for, plus the three it gets whether it asked or
 * not. inotify sends IGNORED, Q_OVERFLOW and UNMOUNT unasked. */
static void emit_masked(struct inst *in, struct watch *w, uint32_t mask, uint32_t cookie,
                        const char *name, int isdir) {
  const uint32_t always = SLIPSTREAM_IN_IGNORED | SLIPSTREAM_IN_Q_OVERFLOW |
                          SLIPSTREAM_IN_UNMOUNT;
  if ((mask & (w->mask | always)) == 0) return;
  emit(in, w->wd, mask | (isdir ? SLIPSTREAM_IN_ISDIR : 0u), cookie, name);
}

static void entry_free(struct entry *e) {
  free(e->name);
  e->name = NULL;
}

static void watch_free(struct watch *w) {
  for (size_t i = 0; i < w->nkids; i++) entry_free(&w->kids[i]);
  free(w->kids);
  free(w->path);
  w->kids = NULL;
  w->path = NULL;
  w->nkids = 0;
}

static struct entry *kid_by_name(struct watch *w, const char *name) {
  for (size_t i = 0; i < w->nkids; i++) {
    if (w->kids[i].name != NULL && strcmp(w->kids[i].name, name) == 0) return &w->kids[i];
  }
  return NULL;
}

static void fill(struct entry *e, const struct stat *st) {
  e->ino = (uint64_t) st->st_ino;
  e->mtime_sec = (int64_t) st->st_mtime;
#if defined(__APPLE__)
  e->mtime_nsec = st->st_mtimespec.tv_nsec;
#else
  e->mtime_nsec = st->st_mtim.tv_nsec;
#endif
  e->size = (uint64_t) st->st_size;
  e->mode = (uint32_t) st->st_mode;
}

/* One look at one watch, and every record the difference asks for. */
static void look(struct inst *in, struct watch *w) {
  struct stat st;
  DIR *d;
  struct dirent *de;
  struct entry *fresh = NULL;
  size_t nfresh = 0;
  size_t cap = 0;

  if (w->dead) return;

  if (stat(w->path, &st) != 0) {
    /* Gone. inotify says DELETE_SELF and then IGNORED, and forgets the
     * watch - so this one stops looking too. */
    emit_masked(in, w, SLIPSTREAM_IN_DELETE_SELF, 0, NULL, w->is_dir);
    emit(in, w->wd, SLIPSTREAM_IN_IGNORED, 0, NULL);
    w->dead = 1;
    return;
  }
  if ((uint64_t) st.st_ino != w->self_ino) {
    /* Another thing stands under that name now. Renaming the watched
     * directory away and making a new one is MOVE_SELF in inotify's
     * terms, and the watch follows the OLD one - which we cannot do by
     * name, so this reports the move and starts again from what is here. */
    emit_masked(in, w, SLIPSTREAM_IN_MOVE_SELF, 0, NULL, w->is_dir);
    w->self_ino = (uint64_t) st.st_ino;
  }

  if (!w->is_dir) {
    struct entry now;
    fill(&now, &st);
    if (now.mtime_sec != w->self_mtime_sec || now.mtime_nsec != w->self_mtime_nsec ||
        now.size != w->self_size) {
      emit_masked(in, w, SLIPSTREAM_IN_MODIFY, 0, NULL, 0);
    } else if (now.mode != w->self_mode) {
      emit_masked(in, w, SLIPSTREAM_IN_ATTRIB, 0, NULL, 0);
    }
    w->self_mtime_sec = now.mtime_sec;
    w->self_mtime_nsec = now.mtime_nsec;
    w->self_size = now.size;
    w->self_mode = now.mode;
    return;
  }

  /* Every name starts this look unseen. The flag says "this look found
   * it", and one that stayed set from the look before would make a
   * deleted name look present forever. */
  for (size_t i = 0; i < w->nkids; i++) w->kids[i].seen = 0;

  d = opendir(w->path);
  if (d == NULL) return;
  for (;;) {
    char full[4096];
    struct stat kst;
    struct entry *had;
    de = readdir(d);
    if (de == NULL) break;
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
    if (snprintf(full, sizeof(full), "%s/%s", w->path, de->d_name) >= (int) sizeof(full)) {
      continue;
    }
    if (lstat(full, &kst) != 0) continue;
    if (nfresh == cap) {
      const size_t want = cap == 0 ? 16 : cap * 2;
      struct entry *grown = realloc(fresh, want * sizeof(*fresh));
      if (grown == NULL) break;
      fresh = grown;
      cap = want;
    }
    memset(&fresh[nfresh], 0, sizeof(fresh[nfresh]));
    fresh[nfresh].name = strdup(de->d_name);
    if (fresh[nfresh].name == NULL) break;
    fill(&fresh[nfresh], &kst);
    had = kid_by_name(w, de->d_name);
    if (had == NULL) {
      /* New under this name. Whether it is a create or the landing half
       * of a rename is decided below, once every name is in. */
      fresh[nfresh].seen = 0;
    } else {
      had->seen = 1;
      fresh[nfresh].seen = 1;
      if (had->ino != fresh[nfresh].ino) {
        /* Same name, other file: what was there is gone and something
         * else took the name. inotify sees the two operations that did
         * that, and so does this. */
        emit_masked(in, w, SLIPSTREAM_IN_DELETE, 0, de->d_name, S_ISDIR(had->mode));
        emit_masked(in, w, SLIPSTREAM_IN_CREATE, 0, de->d_name, S_ISDIR(kst.st_mode));
      } else if (had->mtime_sec != fresh[nfresh].mtime_sec ||
                 had->mtime_nsec != fresh[nfresh].mtime_nsec ||
                 had->size != fresh[nfresh].size) {
        emit_masked(in, w, SLIPSTREAM_IN_MODIFY, 0, de->d_name, S_ISDIR(kst.st_mode));
      } else if (had->mode != fresh[nfresh].mode) {
        emit_masked(in, w, SLIPSTREAM_IN_ATTRIB, 0, de->d_name, S_ISDIR(kst.st_mode));
      }
    }
    nfresh++;
  }
  closedir(d);

  /* A name that was there and is not is either a delete, or the leaving
   * half of a rename inside this directory - and the inode says which.
   * inotify pairs those two records with one cookie, and so does this. */
  for (size_t i = 0; i < w->nkids; i++) {
    struct entry *old = &w->kids[i];
    struct entry *landed = NULL;
    if (old->name == NULL || old->seen) continue;
    for (size_t k = 0; k < nfresh; k++) {
      if (!fresh[k].seen && fresh[k].ino == old->ino) {
        landed = &fresh[k];
        break;
      }
    }
    if (landed != NULL) {
      const uint32_t cookie = ++in->next_cookie;
      const int isdir = S_ISDIR(old->mode);
      emit_masked(in, w, SLIPSTREAM_IN_MOVED_FROM, cookie, old->name, isdir);
      emit_masked(in, w, SLIPSTREAM_IN_MOVED_TO, cookie, landed->name, isdir);
      landed->seen = 1;
    } else {
      emit_masked(in, w, SLIPSTREAM_IN_DELETE, 0, old->name, S_ISDIR(old->mode));
    }
  }
  /* What is left is new and came from nowhere this directory can see. */
  for (size_t k = 0; k < nfresh; k++) {
    if (fresh[k].seen) continue;
    emit_masked(in, w, SLIPSTREAM_IN_CREATE, 0, fresh[k].name, S_ISDIR(fresh[k].mode));
  }

  for (size_t i = 0; i < w->nkids; i++) entry_free(&w->kids[i]);
  free(w->kids);
  w->kids = fresh;
  w->nkids = nfresh;
}

static int looker(void *arg) {
  struct inst *in = (struct inst *) arg;
  for (;;) {
    struct timespec nap;
    mtx_lock(&in->lock);
    if (in->stopping) {
      mtx_unlock(&in->lock);
      return 0;
    }
    for (size_t i = 0; i < in->nwatches; i++) look(in, &in->watches[i]);
    mtx_unlock(&in->lock);
    nap.tv_sec = SLIPSTREAM_INOTIFY_INTERVAL_MS / 1000;
    nap.tv_nsec = (long) (SLIPSTREAM_INOTIFY_INTERVAL_MS % 1000) * 1000000L;
    thrd_sleep(&nap, NULL);
  }
}

int slipstream_inotify_init1(int flags) {
  struct inst *in = NULL;
  int sp[2];

  if ((flags & ~(SLIPSTREAM_IN_CLOEXEC | SLIPSTREAM_IN_NONBLOCK)) != 0) return -EINVAL;
  call_once(&g_once, inotify_once);
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) return -errno;
  if (flags & SLIPSTREAM_IN_CLOEXEC) {
    fcntl(sp[0], F_SETFD, FD_CLOEXEC);
    fcntl(sp[1], F_SETFD, FD_CLOEXEC);
  }
  if (flags & SLIPSTREAM_IN_NONBLOCK) {
    fcntl(sp[0], F_SETFL, fcntl(sp[0], F_GETFL, 0) | O_NONBLOCK);
  }
  /* The writer never waits: a full pipe is an overflow, which is a
   * record, not a stall of the thread that looks. */
  fcntl(sp[1], F_SETFL, fcntl(sp[1], F_GETFL, 0) | O_NONBLOCK);

  mtx_lock(&g_lock);
  for (unsigned i = 0; i < SLIPSTREAM_INOTIFY_MAX; i++) {
    if (!g_inst[i].used) {
      in = &g_inst[i];
      break;
    }
  }
  if (in == NULL) {
    mtx_unlock(&g_lock);
    close(sp[0]);
    close(sp[1]);
    return -EMFILE;
  }
  memset(in, 0, sizeof(*in));
  in->rd = sp[0];
  in->wr = sp[1];
  in->used = 1;
  in->next_wd = 1;
  mtx_init(&in->lock, mtx_plain);
  if (thrd_create(&in->thread, looker, in) != thrd_success) {
    mtx_destroy(&in->lock);
    in->used = 0;
    mtx_unlock(&g_lock);
    close(sp[0]);
    close(sp[1]);
    return -EAGAIN;
  }
  mtx_unlock(&g_lock);
  return in->rd;
}

int slipstream_inotify_add_watch(int fd, const char *path, uint32_t mask) {
  struct inst *in;
  struct watch *grown;
  struct watch w;
  struct stat st;
  int wd;

  if (path == NULL) return -EFAULT;
  if ((mask & SLIPSTREAM_IN_ALL_EVENTS) == 0) return -EINVAL;
  call_once(&g_once, inotify_once);
  mtx_lock(&g_lock);
  in = inst_of(fd);
  mtx_unlock(&g_lock);
  if (in == NULL) return -EBADF;
  if (stat(path, &st) != 0) return -errno;

  mtx_lock(&in->lock);
  /* A path already watched answers the same wd and takes the new mask,
   * which is what inotify does. */
  for (size_t i = 0; i < in->nwatches; i++) {
    if (!in->watches[i].dead && strcmp(in->watches[i].path, path) == 0) {
      in->watches[i].mask = mask;
      wd = in->watches[i].wd;
      mtx_unlock(&in->lock);
      return wd;
    }
  }
  memset(&w, 0, sizeof(w));
  w.path = strdup(path);
  if (w.path == NULL) {
    mtx_unlock(&in->lock);
    return -ENOMEM;
  }
  w.wd = in->next_wd++;
  w.mask = mask;
  w.is_dir = S_ISDIR(st.st_mode);
  w.self_ino = (uint64_t) st.st_ino;
  w.self_mode = (uint32_t) st.st_mode;
  w.self_size = (uint64_t) st.st_size;
  w.self_mtime_sec = (int64_t) st.st_mtime;
#if defined(__APPLE__)
  w.self_mtime_nsec = st.st_mtimespec.tv_nsec;
#else
  w.self_mtime_nsec = st.st_mtim.tv_nsec;
#endif
  grown = realloc(in->watches, (in->nwatches + 1) * sizeof(*in->watches));
  if (grown == NULL) {
    free(w.path);
    mtx_unlock(&in->lock);
    return -ENOMEM;
  }
  in->watches = grown;
  in->watches[in->nwatches] = w;
  /* The first look is taken NOW, not one interval from now: everything
   * that is there already is what the watch starts from, so nothing
   * that was there before the watch is reported as new. */
  if (w.is_dir) {
    DIR *d = opendir(path);
    struct watch *self = &in->watches[in->nwatches];
    if (d != NULL) {
      struct dirent *de;
      size_t cap = 0;
      while ((de = readdir(d)) != NULL) {
        char full[4096];
        struct stat kst;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        if (snprintf(full, sizeof(full), "%s/%s", path, de->d_name) >= (int) sizeof(full)) {
          continue;
        }
        if (lstat(full, &kst) != 0) continue;
        if (self->nkids == cap) {
          const size_t want = cap == 0 ? 16 : cap * 2;
          struct entry *k = realloc(self->kids, want * sizeof(*k));
          if (k == NULL) break;
          self->kids = k;
          cap = want;
        }
        memset(&self->kids[self->nkids], 0, sizeof(self->kids[self->nkids]));
        self->kids[self->nkids].name = strdup(de->d_name);
        if (self->kids[self->nkids].name == NULL) break;
        fill(&self->kids[self->nkids], &kst);
        self->nkids++;
      }
      closedir(d);
    }
  }
  wd = in->watches[in->nwatches].wd;
  in->nwatches++;
  mtx_unlock(&in->lock);
  return wd;
}

int slipstream_inotify_rm_watch(int fd, int wd) {
  struct inst *in;
  call_once(&g_once, inotify_once);
  mtx_lock(&g_lock);
  in = inst_of(fd);
  mtx_unlock(&g_lock);
  if (in == NULL) return -EBADF;

  mtx_lock(&in->lock);
  for (size_t i = 0; i < in->nwatches; i++) {
    if (in->watches[i].wd != wd || in->watches[i].dead) continue;
    /* inotify sends one IGNORED for the watch that goes. */
    emit(in, wd, SLIPSTREAM_IN_IGNORED, 0, NULL);
    watch_free(&in->watches[i]);
    memmove(&in->watches[i], &in->watches[i + 1],
            (in->nwatches - i - 1) * sizeof(*in->watches));
    in->nwatches--;
    mtx_unlock(&in->lock);
    return 0;
  }
  mtx_unlock(&in->lock);
  return -EINVAL;
}

int slipstream_inotify_close(int fd) {
  struct inst *in;
  int rd;
  int wr;
  call_once(&g_once, inotify_once);
  mtx_lock(&g_lock);
  in = inst_of(fd);
  if (in == NULL) {
    mtx_unlock(&g_lock);
    return -EBADF;
  }
  mtx_lock(&in->lock);
  in->stopping = 1;
  mtx_unlock(&in->lock);
  mtx_unlock(&g_lock);

  thrd_join(in->thread, NULL);

  mtx_lock(&g_lock);
  for (size_t i = 0; i < in->nwatches; i++) watch_free(&in->watches[i]);
  free(in->watches);
  rd = in->rd;
  wr = in->wr;
  mtx_destroy(&in->lock);
  memset(in, 0, sizeof(*in));
  mtx_unlock(&g_lock);
  close(wr);
  return close(rd) == 0 ? 0 : -errno;
}

/* ---- Windows: ReadDirectoryChangesW, next --------------------------- */
#else

int slipstream_inotify_init1(int flags) {
  (void) flags;
  return -ENOSYS;
}

int slipstream_inotify_add_watch(int fd, const char *path, uint32_t mask) {
  (void) fd;
  (void) path;
  (void) mask;
  return -ENOSYS;
}

int slipstream_inotify_rm_watch(int fd, int wd) {
  (void) fd;
  (void) wd;
  return -ENOSYS;
}

int slipstream_inotify_close(int fd) {
  (void) fd;
  return -ENOSYS;
}

#endif
