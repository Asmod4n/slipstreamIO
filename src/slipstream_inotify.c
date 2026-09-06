/* slipstream_inotify.h, three times. See that header for the contract,
 * and for the one rule that decides everything here: a caller cannot
 * see which arm answered.
 *
 * NO ARM MAKES A THREAD. Each one uses the mechanism its own system has
 * for the job - inotify on Linux, EVFILT_VNODE on the BSDs and macOS,
 * ReadDirectoryChangesW on Windows - and the work of turning what that
 * mechanism says into inotify's records happens where the caller reads,
 * on the caller's own thread. A system with no such mechanism gets
 * -ENOSYS, because a watch that polls the disk behind a caller's back
 * is not the same thing and would not say so.
 */
/* inotify, kqueue and the dirent calls sit behind these under a strict
 * -std=c11, which is what this Makefile builds with. Declared on the
 * first lines, as engine_posix.c does. */
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

int slipstream_inotify_read(int fd, void *buf, unsigned len) {
  ssize_t got;
  if (buf == NULL) return -EFAULT;
  if (len < sizeof(struct slipstream_inotify_event)) return -EINVAL;
  got = read(fd, buf, len);
  if (got >= 0) return (int) got;
  if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
  return -errno;
}

/* ---- The BSDs and macOS: EVFILT_VNODE ---------------------------------
 *
 * kqueue says THAT a directory changed, never what: NOTE_WRITE on a
 * directory means a name appeared, went, or was renamed inside it. So
 * the names come from looking at the directory once, when the kernel
 * says it changed - not from looking at it over and over. That is the
 * whole difference to a poller, and it is why this arm can carry
 * inotify's records without a thread and without a timer.
 *
 * SLIPSTREAM_INOTIFY_KQUEUE builds this arm on a Linux host, where
 * libkqueue provides the API - the same reason slipstream_signal has
 * SLIPSTREAM_SIGNAL_NO_SIGNALFD. An arm nobody can run is an arm nobody
 * has checked.
 */
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || \
    defined(__DragonFly__) || defined(__APPLE__) || defined(SLIPSTREAM_INOTIFY_KQUEUE)

#include <dirent.h>
#include <fcntl.h>
#include <sys/event.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* macOS has a mode for exactly this: open a thing to watch it, not to
 * read it. Everywhere else O_RDONLY is what a directory takes. */
#ifdef O_EVTONLY
#define WATCH_OPEN_FLAGS (O_EVTONLY)
#else
#define WATCH_OPEN_FLAGS (O_RDONLY)
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
  int fd;
  char *path;
  uint32_t mask;
  int is_dir;
  struct entry *kids;
  size_t nkids;
};

/* Records made but not yet taken. One kqueue event about a directory
 * can mean several records, and a caller's buffer may hold fewer, so
 * what is made is kept here until it is read. This is inotify's own
 * queue, in the only place this arm can keep one. */
struct inst {
  int kq;
  int used;
  int nonblock;
  int next_wd;
  uint32_t next_cookie;
  char *queue;
  size_t qlen;
  size_t qcap;
  int overflowed;
  struct watch *watches;
  size_t nwatches;
};

/* inotify's queue is bounded, and a caller that reads slowly loses
 * events and hears IN_Q_OVERFLOW. This one is bounded the same way, for
 * the same reason: an unbounded queue turns a slow reader into memory
 * that never comes back. */
#ifndef SLIPSTREAM_INOTIFY_QUEUE_MAX
#define SLIPSTREAM_INOTIFY_QUEUE_MAX (1u << 20)
#endif

#define SLIPSTREAM_INOTIFY_MAX 64
static struct inst g_inst[SLIPSTREAM_INOTIFY_MAX];

static struct inst *inst_of(int fd) {
  for (unsigned i = 0; i < SLIPSTREAM_INOTIFY_MAX; i++) {
    if (g_inst[i].used && g_inst[i].kq == fd) return &g_inst[i];
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

static void emit(struct inst *in, int wd, uint32_t mask, uint32_t cookie, const char *name) {
  const uint32_t len = name_len(name);
  const size_t total = sizeof(struct slipstream_inotify_event) + len;
  struct slipstream_inotify_event ev;

  if (in->qlen + total > SLIPSTREAM_INOTIFY_QUEUE_MAX) {
    /* inotify sends ONE overflow record for a run of lost events, with
     * wd -1 and no name, and sends it again only after one gets through. */
    if (in->overflowed) return;
    if (in->qlen + sizeof(ev) > SLIPSTREAM_INOTIFY_QUEUE_MAX) return;
    ev.wd = -1;
    ev.mask = SLIPSTREAM_IN_Q_OVERFLOW;
    ev.cookie = 0;
    ev.len = 0;
    memcpy(in->queue + in->qlen, &ev, sizeof(ev));
    in->qlen += sizeof(ev);
    in->overflowed = 1;
    return;
  }
  if (in->qlen + total > in->qcap) {
    size_t want = in->qcap == 0 ? 4096 : in->qcap * 2;
    char *grown;
    while (want < in->qlen + total) want *= 2;
    grown = realloc(in->queue, want);
    if (grown == NULL) return;
    in->queue = grown;
    in->qcap = want;
  }
  ev.wd = (int32_t) wd;
  ev.mask = mask;
  ev.cookie = cookie;
  ev.len = len;
  memcpy(in->queue + in->qlen, &ev, sizeof(ev));
  in->qlen += sizeof(ev);
  if (len != 0) {
    memset(in->queue + in->qlen, 0, len);
    memcpy(in->queue + in->qlen, name, strlen(name));
    in->qlen += len;
  }
  in->overflowed = 0;
}

/* What a watch asked for, plus the three it gets whether it asked or
 * not. inotify sends IGNORED, Q_OVERFLOW and UNMOUNT unasked. */
static void emit_masked(struct inst *in, struct watch *w, uint32_t mask, uint32_t cookie,
                        const char *name, int isdir) {
  const uint32_t always =
      SLIPSTREAM_IN_IGNORED | SLIPSTREAM_IN_Q_OVERFLOW | SLIPSTREAM_IN_UNMOUNT;
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
  if (w->fd >= 0) close(w->fd);
  w->kids = NULL;
  w->path = NULL;
  w->fd = -1;
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

/* What a directory holds now, as an array this arm owns. */
static struct entry *scan(const char *path, size_t *count) {
  DIR *d = opendir(path);
  struct entry *fresh = NULL;
  size_t n = 0;
  size_t cap = 0;
  struct dirent *de;

  *count = 0;
  if (d == NULL) return NULL;
  while ((de = readdir(d)) != NULL) {
    char full[4096];
    struct stat st;
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
    if (snprintf(full, sizeof(full), "%s/%s", path, de->d_name) >= (int) sizeof(full)) continue;
    if (lstat(full, &st) != 0) continue;
    if (n == cap) {
      const size_t want = cap == 0 ? 16 : cap * 2;
      struct entry *grown = realloc(fresh, want * sizeof(*grown));
      if (grown == NULL) break;
      fresh = grown;
      cap = want;
    }
    memset(&fresh[n], 0, sizeof(fresh[n]));
    fresh[n].name = strdup(de->d_name);
    if (fresh[n].name == NULL) break;
    fill(&fresh[n], &st);
    n++;
  }
  closedir(d);
  *count = n;
  return fresh;
}

/* The kernel said this directory changed. What changed is the
 * difference between what it holds and what it held. */
static void directory_changed(struct inst *in, struct watch *w) {
  size_t nfresh = 0;
  struct entry *fresh = scan(w->path, &nfresh);

  if (fresh == NULL && nfresh == 0) return;
  for (size_t i = 0; i < w->nkids; i++) w->kids[i].seen = 0;

  for (size_t k = 0; k < nfresh; k++) {
    struct entry *had = kid_by_name(w, fresh[k].name);
    if (had == NULL) continue;
    had->seen = 1;
    fresh[k].seen = 1;
    if (had->ino != fresh[k].ino) {
      /* Same name, other file: what was there is gone and something
       * else took the name. inotify sees the two operations that did
       * that, and so does this. */
      emit_masked(in, w, SLIPSTREAM_IN_DELETE, 0, had->name, S_ISDIR(had->mode));
      emit_masked(in, w, SLIPSTREAM_IN_CREATE, 0, fresh[k].name, S_ISDIR(fresh[k].mode));
    } else if (had->mtime_sec != fresh[k].mtime_sec || had->mtime_nsec != fresh[k].mtime_nsec ||
               had->size != fresh[k].size) {
      emit_masked(in, w, SLIPSTREAM_IN_MODIFY, 0, fresh[k].name, S_ISDIR(fresh[k].mode));
    } else if (had->mode != fresh[k].mode) {
      emit_masked(in, w, SLIPSTREAM_IN_ATTRIB, 0, fresh[k].name, S_ISDIR(fresh[k].mode));
    }
  }

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
  for (size_t k = 0; k < nfresh; k++) {
    if (fresh[k].seen) continue;
    emit_masked(in, w, SLIPSTREAM_IN_CREATE, 0, fresh[k].name, S_ISDIR(fresh[k].mode));
  }

  for (size_t i = 0; i < w->nkids; i++) entry_free(&w->kids[i]);
  free(w->kids);
  w->kids = fresh;
  w->nkids = nfresh;
}

static void forget(struct inst *in, size_t at) {
  watch_free(&in->watches[at]);
  memmove(&in->watches[at], &in->watches[at + 1],
          (in->nwatches - at - 1) * sizeof(*in->watches));
  in->nwatches--;
}

/* One kqueue event, as inotify records. */
static void translate(struct inst *in, const struct kevent *ke) {
  struct watch *w = NULL;
  size_t at = 0;
  for (size_t i = 0; i < in->nwatches; i++) {
    if (in->watches[i].fd == (int) ke->ident) {
      w = &in->watches[i];
      at = i;
      break;
    }
  }
  if (w == NULL) return;

  if (ke->fflags & (NOTE_WRITE | NOTE_EXTEND | NOTE_LINK)) {
    if (w->is_dir) {
      directory_changed(in, w);
    } else {
      emit_masked(in, w, SLIPSTREAM_IN_MODIFY, 0, NULL, 0);
    }
  }
  if (ke->fflags & NOTE_ATTRIB) emit_masked(in, w, SLIPSTREAM_IN_ATTRIB, 0, NULL, w->is_dir);
  if (ke->fflags & NOTE_RENAME) emit_masked(in, w, SLIPSTREAM_IN_MOVE_SELF, 0, NULL, w->is_dir);
  if (ke->fflags & (NOTE_DELETE | NOTE_REVOKE)) {
    /* Gone. inotify says DELETE_SELF and then IGNORED, and forgets the
     * watch - so this one forgets it too, which also takes it off the
     * kqueue, because closing the descriptor does that. */
    emit_masked(in, w, SLIPSTREAM_IN_DELETE_SELF, 0, NULL, w->is_dir);
    emit(in, w->wd, SLIPSTREAM_IN_IGNORED, 0, NULL);
    forget(in, at);
  }
}

/* Every event the kernel has for this instance right now, turned into
 * records. Nothing waits here: waiting is the caller's read. */
static void take_events(struct inst *in, const struct timespec *wait) {
  struct kevent evs[32];
  int got;
  do {
    got = kevent(in->kq, NULL, 0, evs, 32, wait);
    if (got <= 0) return;
    for (int i = 0; i < got; i++) translate(in, &evs[i]);
    /* Only the first pass may wait; what is already there is taken
     * without waiting again. */
    wait = &(struct timespec){0, 0};
  } while (got == 32);
}

int slipstream_inotify_init1(int flags) {
  struct inst *in = NULL;
  int kq;

  if ((flags & ~(SLIPSTREAM_IN_CLOEXEC | SLIPSTREAM_IN_NONBLOCK)) != 0) return -EINVAL;
  for (unsigned i = 0; i < SLIPSTREAM_INOTIFY_MAX; i++) {
    if (!g_inst[i].used) {
      in = &g_inst[i];
      break;
    }
  }
  if (in == NULL) return -EMFILE;
  kq = kqueue();
  if (kq < 0) return -errno;
  if (flags & SLIPSTREAM_IN_CLOEXEC) fcntl(kq, F_SETFD, FD_CLOEXEC);
  memset(in, 0, sizeof(*in));
  in->kq = kq;
  in->used = 1;
  in->next_wd = 1;
  in->nonblock = (flags & SLIPSTREAM_IN_NONBLOCK) != 0;
  return kq;
}

int slipstream_inotify_add_watch(int fd, const char *path, uint32_t mask) {
  struct inst *in = inst_of(fd);
  struct watch w;
  struct watch *grown;
  struct kevent ke;
  struct stat st;
  int wfd;

  if (path == NULL) return -EFAULT;
  if ((mask & SLIPSTREAM_IN_ALL_EVENTS) == 0) return -EINVAL;
  if (in == NULL) return -EBADF;
  if (stat(path, &st) != 0) return -errno;

  /* A path already watched answers the same wd and takes the new mask,
   * which is what inotify does. */
  for (size_t i = 0; i < in->nwatches; i++) {
    if (strcmp(in->watches[i].path, path) == 0) {
      in->watches[i].mask = mask;
      return in->watches[i].wd;
    }
  }

  wfd = open(path, WATCH_OPEN_FLAGS);
  if (wfd < 0) return -errno;
  fcntl(wfd, F_SETFD, FD_CLOEXEC);

  memset(&w, 0, sizeof(w));
  w.fd = wfd;
  w.path = strdup(path);
  if (w.path == NULL) {
    close(wfd);
    return -ENOMEM;
  }
  w.wd = in->next_wd;
  w.mask = mask;
  w.is_dir = S_ISDIR(st.st_mode);
  /* The first look is taken NOW, so what is there already is what the
   * watch starts from and nothing that came before it is reported. */
  if (w.is_dir) w.kids = scan(path, &w.nkids);

  EV_SET(&ke, (uintptr_t) wfd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
         NOTE_WRITE | NOTE_EXTEND | NOTE_LINK | NOTE_ATTRIB | NOTE_DELETE | NOTE_RENAME |
             NOTE_REVOKE,
         0, NULL);
  if (kevent(in->kq, &ke, 1, NULL, 0, NULL) < 0) {
    const int why = errno;
    free(w.path);
    close(wfd);
    return -why;
  }
  grown = realloc(in->watches, (in->nwatches + 1) * sizeof(*in->watches));
  if (grown == NULL) {
    free(w.path);
    close(wfd);
    return -ENOMEM;
  }
  in->watches = grown;
  in->watches[in->nwatches] = w;
  in->nwatches++;
  in->next_wd++;
  return w.wd;
}

int slipstream_inotify_rm_watch(int fd, int wd) {
  struct inst *in = inst_of(fd);
  if (in == NULL) return -EBADF;
  for (size_t i = 0; i < in->nwatches; i++) {
    if (in->watches[i].wd != wd) continue;
    /* inotify sends one IGNORED for the watch that goes. Closing the
     * descriptor takes it off the kqueue, which is what kqueue(2) says. */
    emit(in, wd, SLIPSTREAM_IN_IGNORED, 0, NULL);
    forget(in, i);
    return 0;
  }
  return -EINVAL;
}

int slipstream_inotify_read(int fd, void *buf, unsigned len) {
  struct inst *in = inst_of(fd);
  struct timespec zero = {0, 0};
  size_t out = 0;

  if (buf == NULL) return -EFAULT;
  if (in == NULL) return -EBADF;
  if (len < sizeof(struct slipstream_inotify_event)) return -EINVAL;

  if (in->qlen == 0) take_events(in, in->nonblock ? &zero : NULL);
  if (in->qlen == 0) return 0;

  /* Whole records only, as inotify gives. */
  while (out < in->qlen) {
    struct slipstream_inotify_event ev;
    size_t total;
    memcpy(&ev, in->queue + out, sizeof(ev));
    total = sizeof(ev) + ev.len;
    if (out + total > len) break;
    out += total;
  }
  if (out == 0) return -EINVAL;
  memcpy(buf, in->queue, out);
  memmove(in->queue, in->queue + out, in->qlen - out);
  in->qlen -= out;
  return (int) out;
}

int slipstream_inotify_close(int fd) {
  struct inst *in = inst_of(fd);
  int kq;
  if (in == NULL) return -EBADF;
  while (in->nwatches != 0) forget(in, in->nwatches - 1);
  free(in->watches);
  free(in->queue);
  kq = in->kq;
  memset(in, 0, sizeof(*in));
  return close(kq) == 0 ? 0 : -errno;
}

/* ---- Everything else: no mechanism, and it says so -------------------
 *
 * Windows has ReadDirectoryChangesW and is the next arm. A system with
 * nothing of the kind answers -ENOSYS rather than polling the disk
 * behind a caller's back: that would be a different thing wearing this
 * name, and slower than the caller knows.
 */
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

int slipstream_inotify_read(int fd, void *buf, unsigned len) {
  (void) fd;
  (void) buf;
  (void) len;
  return -ENOSYS;
}

#endif
