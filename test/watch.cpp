// A multishot poll driven the way a Watcher drives it: armed once, it
// keeps answering; its mask is changed IN PLACE without re-registering;
// and removing it completes the poll rather than leaving it dangling.
//
// Nothing here uses poll(2). That is the point: this engine is select(2),
// so what a poll can report is what select knows - POLLIN and POLLOUT,
// and neither POLLHUP nor POLLERR. A closed peer arrives as readable, and
// the reader finds out by reading, exactly as it does with select.
#include <liburing.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>

static int fail(const char* what) {
  std::printf("watch: FAILED - %s\n", what);
  return 1;
}

// One completion, or a failure - never a wait that does not end.
static int take(struct io_uring* r, struct io_uring_cqe** c) {
  return io_uring_wait_cqe(r, c);
}

int main() {
  alarm(90);  // a wait that never returns is a FAILURE, not a hung test run
  struct io_uring r;
  if (io_uring_queue_init(64, &r, 0) != 0) return fail("queue_init");

  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return fail("socketpair");

  // Armed once for readability, and left alone for the whole test.
  struct io_uring_sqe* s = io_uring_get_sqe(&r);
  io_uring_prep_multishot_poll_add(s, sv[0], POLLIN);
  io_uring_sqe_set_data64(s, 0x7a7a);
  if (io_uring_submit(&r) < 1) return fail("submit poll");

  struct io_uring_cqe* c = nullptr;

  // TWICE, off ONE registration. A oneshot poll answers the first write
  // and goes quiet; this is the whole difference a Watcher rests on.
  for (int i = 0; i < 2; i++) {
    if (write(sv[1], "x", 1) != 1) return fail("write");
    if (take(&r, &c) != 0) return fail("wait readable");
    if (c->user_data != 0x7a7a) return fail("wrong user_data");
    if (!(c->res & POLLIN)) return fail("not readable");
    if (!(c->flags & IORING_CQE_F_MORE)) return fail("multishot did not say MORE");
    io_uring_cqe_seen(&r, c);
    char b;
    if (read(sv[0], &b, 1) != 1) return fail("read back");
  }
  std::printf("multishot poll: two readable events off one registration\n");

  // The mask changes in place. A socketpair end with nothing written to
  // it is writable and not readable, so switching to POLLOUT must start
  // answering with no write from the peer at all.
  s = io_uring_get_sqe(&r);
  io_uring_prep_poll_update(s, 0x7a7a, 0x7a7a, POLLOUT, IORING_POLL_UPDATE_EVENTS);
  io_uring_sqe_set_data64(s, 0xbeef);
  if (io_uring_submit(&r) < 1) return fail("submit update");
  if (take(&r, &c) != 0) return fail("wait update");
  if (c->user_data != 0xbeef || c->res != 0) return fail("update refused");
  io_uring_cqe_seen(&r, c);

  if (take(&r, &c) != 0) return fail("wait writable");
  if (c->user_data != 0x7a7a) return fail("update lost the user_data");
  if (!(c->res & POLLOUT)) return fail("not writable after update");
  if (c->res & POLLIN) return fail("still reporting readable after update");
  io_uring_cqe_seen(&r, c);
  std::printf("poll update: the mask changed in place, same registration\n");

  // And removal ENDS it: the poll completes with -ECANCELED, so whoever
  // armed it hears that it is over instead of waiting forever.
  s = io_uring_get_sqe(&r);
  io_uring_prep_poll_remove(s, 0x7a7a);
  io_uring_sqe_set_data64(s, 0xdead);
  if (io_uring_submit(&r) < 1) return fail("submit remove");

  int saw_cancel = 0, saw_remove = 0;
  for (int i = 0; i < 2; i++) {
    if (take(&r, &c) != 0) return fail("wait remove");
    if (c->user_data == 0x7a7a && c->res == -ECANCELED) saw_cancel = 1;
    if (c->user_data == 0xdead && c->res == 0) saw_remove = 1;
    io_uring_cqe_seen(&r, c);
  }
  if (!saw_cancel) return fail("the removed poll never completed");
  if (!saw_remove) return fail("the removal itself never completed");
  std::printf("poll remove: the armed poll ended with -ECANCELED\n");

  // Removing something that is not there is answered, not ignored.
  s = io_uring_get_sqe(&r);
  io_uring_prep_poll_remove(s, 0x7a7a);
  io_uring_sqe_set_data64(s, 0xfeed);
  if (io_uring_submit(&r) < 1) return fail("submit second remove");
  if (take(&r, &c) != 0) return fail("wait second remove");
  if (c->user_data != 0xfeed || c->res != -ENOENT) return fail("a second remove is not ENOENT");
  io_uring_cqe_seen(&r, c);
  std::printf("poll remove: removing what is gone answers -ENOENT\n");

  // AND THE ONE THAT MATTERS MOST: a byte nobody reads must not cost
  // anything. select is level-triggered, so an armed multishot poll over
  // an unread descriptor used to post for every turn of the engine -
  // 1,591,898 completions and 1.99 seconds of CPU in two seconds of wall
  // clock, for a single byte. A kernel ring is idle here, and so must
  // this be.
  {
    struct io_uring_sqe* q = io_uring_get_sqe(&r);
    io_uring_prep_multishot_poll_add(q, sv[0], POLLIN);
    io_uring_sqe_set_data64(q, 0x5157);
    if (io_uring_submit(&r) < 1) return fail("submit spin poll");
    if (write(sv[1], "x", 1) != 1) return fail("write spin byte");
    if (take(&r, &c) != 0) return fail("wait spin readable");
    io_uring_cqe_seen(&r, c);  // taken, and then deliberately NOT read

    struct rusage before {}, after {};
    getrusage(RUSAGE_SELF, &before);
    sleep(2);
    getrusage(RUSAGE_SELF, &after);
    const long us = (after.ru_utime.tv_sec - before.ru_utime.tv_sec) * 1000000L +
                    (after.ru_utime.tv_usec - before.ru_utime.tv_usec) +
                    (after.ru_stime.tv_sec - before.ru_stime.tv_sec) * 1000000L +
                    (after.ru_stime.tv_usec - before.ru_stime.tv_usec);
    unsigned extra = 0;
    while (io_uring_peek_cqe(&r, &c) == 0) {
      io_uring_cqe_seen(&r, c);
      extra++;
    }
    // Generous on purpose: this is the difference between idle and a
    // pegged core, not a millisecond of tuning.
    if (us > 100000) return fail("an unread byte burns CPU - the poll is spinning");
    if (extra > 4) return fail("an unread byte floods completions");
    std::printf("no spin: 2s over an unread byte cost %ld us and %u completions\n", us, extra);
  }

  // cancel_fd is the ONE cancel: it takes everything armed on a
  // descriptor, which is all a CDATA destructor knows and all it needs.
  // Two polls on one fd, one sentence, both gone.
  {
    for (int i = 0; i < 2; i++) {
      struct io_uring_sqe* q = io_uring_get_sqe(&r);
      io_uring_prep_multishot_poll_add(q, sv[1], POLLOUT);
      io_uring_sqe_set_data64(q, 0xc001 + (unsigned)i);
      if (io_uring_submit(&r) < 1) return fail("submit poll for cancel");
      if (take(&r, &c) != 0) return fail("wait writable before cancel");
      io_uring_cqe_seen(&r, c);  // taken, so the poll goes quiet
    }
    struct io_uring_sqe* q = io_uring_get_sqe(&r);
    io_uring_prep_cancel_fd(q, sv[1], IORING_ASYNC_CANCEL_ALL);
    io_uring_sqe_set_data64(q, 0xca11);
    if (io_uring_submit(&r) < 1) return fail("submit cancel_fd");

    int cancelled = 0, answered = 0;
    for (int i = 0; i < 3; i++) {
      if (take(&r, &c) != 0) return fail("wait cancel_fd");
      if (c->res == -ECANCELED && (c->user_data == 0xc001 || c->user_data == 0xc002)) cancelled++;
      if (c->user_data == 0xca11) {
        if (c->res != 2) return fail("cancel_fd did not report two");
        answered = 1;
      }
      io_uring_cqe_seen(&r, c);
    }
    if (cancelled != 2) return fail("cancel_fd left a poll armed");
    if (!answered) return fail("cancel_fd never completed");
    std::printf("cancel_fd: both polls on one descriptor, one sentence\n");
  }

  close(sv[0]);
  close(sv[1]);
  io_uring_queue_exit(&r);
  std::printf("watch: ok\n");
  return 0;
}
