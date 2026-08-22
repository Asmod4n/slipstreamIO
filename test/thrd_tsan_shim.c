/* A TEST-HARNESS shim, and nothing else: it is linked into the
 * ThreadSanitizer build of the tests and into no other build.
 *
 * WHY IT EXISTS: ThreadSanitizer works by intercepting the pthread
 * symbols. glibc's C11 thread functions do not go through those - they
 * call the internal aliases - so a thread created with thrd_create is a
 * thread TSan never learned about, and the first thing that thread does
 * is dereference the runtime state it does not have. That is not a
 * property of anything in src/: a four-line program whose whole body is
 * thrd_create + thrd_join dies the same way, under gcc and clang alike,
 * while the identical program written with pthread_create is clean.
 *
 * So the choice is between "no TSan verdict on the engine at all" and
 * "route the same calls through the symbols TSan can see". This file is
 * the second. It changes no logic: every function here is the C11 call
 * spelled as its pthread equivalent, which is what glibc does one layer
 * down anyway - glibc's own thrd_t IS pthread_t and its mtx_t/cnd_t are
 * the pthread objects, so the casts below are re-spellings and not
 * conversions.
 *
 * It is also, not coincidentally, the shape of the shim macOS will need
 * for real: Apple has never shipped <threads.h> (see TASKS.md). Here it
 * is a test artifact; there it would be part of that platform's answer. */
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <threads.h>

struct slipstream_trampoline {
  thrd_start_t func;
  void *arg;
};

static void *slipstream_thrd_entry(void *p) {
  struct slipstream_trampoline t = *(struct slipstream_trampoline *)p;
  free(p);
  return (void *)(intptr_t)t.func(t.arg);
}

int thrd_create(thrd_t *thr, thrd_start_t func, void *arg) {
  struct slipstream_trampoline *t =
      (struct slipstream_trampoline *)malloc(sizeof(struct slipstream_trampoline));
  if (!t) return thrd_nomem;
  t->func = func;
  t->arg = arg;
  if (pthread_create((pthread_t *)thr, NULL, slipstream_thrd_entry, t) != 0) {
    free(t);
    return thrd_error;
  }
  return thrd_success;
}

int thrd_join(thrd_t thr, int *res) {
  void *r = NULL;
  if (pthread_join((pthread_t)thr, &r) != 0) return thrd_error;
  if (res) *res = (int)(intptr_t)r;
  return thrd_success;
}

int mtx_init(mtx_t *m, int type) {
  (void)type; /* only mtx_plain is used */
  return pthread_mutex_init((pthread_mutex_t *)m, NULL) == 0 ? thrd_success : thrd_error;
}
int mtx_lock(mtx_t *m) {
  return pthread_mutex_lock((pthread_mutex_t *)m) == 0 ? thrd_success : thrd_error;
}
int mtx_unlock(mtx_t *m) {
  return pthread_mutex_unlock((pthread_mutex_t *)m) == 0 ? thrd_success : thrd_error;
}
void mtx_destroy(mtx_t *m) { pthread_mutex_destroy((pthread_mutex_t *)m); }

int cnd_init(cnd_t *c) {
  return pthread_cond_init((pthread_cond_t *)c, NULL) == 0 ? thrd_success : thrd_error;
}
int cnd_wait(cnd_t *c, mtx_t *m) {
  return pthread_cond_wait((pthread_cond_t *)c, (pthread_mutex_t *)m) == 0 ? thrd_success
                                                                           : thrd_error;
}
int cnd_broadcast(cnd_t *c) {
  return pthread_cond_broadcast((pthread_cond_t *)c) == 0 ? thrd_success : thrd_error;
}
int cnd_signal(cnd_t *c) {
  return pthread_cond_signal((pthread_cond_t *)c) == 0 ? thrd_success : thrd_error;
}
void cnd_destroy(cnd_t *c) { pthread_cond_destroy((pthread_cond_t *)c); }
