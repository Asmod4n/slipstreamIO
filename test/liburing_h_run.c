/* liburing.h's inline half, RUN and not only compiled. The same
 * program builds natively and with MinGW; test/liburing_h_shims.sh runs
 * the second one under Wine when both are installed. What it exercises
 * is exactly what the shims must not bend: SQE/CQE layout, the prep
 * fields, the SQ/CQ cursor math across a mask boundary, the CMSG
 * pointer walk, and - measured, per side - what liburing.h's
 * (unsigned long) pointer casts do to a 64-bit address: carried in full
 * where long is 64 bits, truncated on Win64. The day upstream fixes
 * that cast, the Windows half here fails and the README loses its
 * sentence about it. */
#include <liburing.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

static int failed;
static void check(int ok, const char *what) {
  printf("  %-56s %s\n", what, ok ? "ok" : "FAILED");
  if (!ok) failed = 1;
}

int main(void) {
  check(sizeof(struct io_uring_sqe) == 64, "an SQE is 64 bytes");
  check(sizeof(struct io_uring_cqe) == 16, "a CQE is 16 bytes");
  check(__swahw32(0x12345678u) == 0x56781234u, "__swahw32 swaps the halfwords");

  /* A ring laid out by hand - khead/ktail into locals, arrays for the
   * entries - which is all the header's inlines ever look at. */
  static struct io_uring_sqe sqes[4];
  static struct io_uring_cqe cqes[8];
  unsigned sq_head = 0, sq_tail = 0, sq_flags = 0, sq_dropped = 0;
  unsigned cq_head = 0, cq_tail = 0;
  unsigned sq_array[4] = { 0, 1, 2, 3 };
  struct io_uring ring;
  memset(&ring, 0, sizeof(ring));
  ring.sq.khead = &sq_head;
  ring.sq.ktail = &sq_tail;
  ring.sq.kflags = &sq_flags;
  ring.sq.kdropped = &sq_dropped;
  ring.sq.array = sq_array;
  ring.sq.sqes = sqes;
  ring.sq.ring_mask = 3;
  ring.sq.ring_entries = 4;
  ring.cq.khead = &cq_head;
  ring.cq.ktail = &cq_tail;
  ring.cq.cqes = cqes;
  ring.cq.ring_mask = 7;
  ring.cq.ring_entries = 8;

  struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
  check(sqe == &sqes[0], "get_sqe hands out slot 0");
  io_uring_prep_read(sqe, 5, NULL, 123, 77);
  io_uring_sqe_set_data64(sqe, 0xabc);
  io_uring_sqe_set_flags(sqe, IOSQE_IO_LINK);
  check(sqe->opcode == IORING_OP_READ && sqe->fd == 5 && sqe->len == 123 &&
            sqe->off == 77 && sqe->user_data == 0xabc && sqe->flags == IOSQE_IO_LINK,
        "prep_read fills opcode/fd/len/off, data and link ride along");

  char buf[16];
  sqe = io_uring_get_sqe(&ring);
  check(sqe == &sqes[1], "the next SQE is slot 1");
  io_uring_prep_send(sqe, 7, buf, sizeof(buf), 0x4000);
  check(sqe->opcode == IORING_OP_SEND && sqe->msg_flags == 0x4000,
        "prep_send carries the socket flags");

  /* What the pointer cast does, measured on this side of the fence. */
#ifdef _WIN32
  void *high = VirtualAlloc((void *) (uintptr_t) 0x200000000ULL, 4096,
                            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (high != NULL) {
    sqe = io_uring_get_sqe(&ring);
    io_uring_prep_read(sqe, 3, high, 8, 0);
    check(sqe->addr != (uintptr_t) high,
          "Win64: the addr cast truncated a >4G pointer (upstream LP64)");
    check((unsigned) sqe->addr == (unsigned) (uintptr_t) high,
          "and only the low 32 bits survived");
  } else {
    printf("  no allocation above 4G granted - truncation not measurable\n");
  }
#else
  sqe = io_uring_get_sqe(&ring);
  io_uring_prep_read(sqe, 3, buf, 8, 0);
  check(sqe->addr == (uintptr_t) buf, "LP64: the addr cast carries the pointer whole");
#endif

  /* Three completions posted by hand across the mask boundary: the walk
   * must mask, stay in order, and a partial advance must leave the rest. */
  cq_head = 7;
  cq_tail = 7;
  cqes[7].user_data = 1;
  cqes[7].res = 11;
  cqes[0].user_data = 2;
  cqes[0].res = 22;
  cqes[1].user_data = 3;
  cqes[1].res = 33;
  cq_tail = 10;
  check(io_uring_cq_ready(&ring) == 3, "cq_ready sees the three");

  unsigned head;
  struct io_uring_cqe *cqe;
  uint64_t seen[3];
  unsigned n = 0;
  io_uring_for_each_cqe(&ring, head, cqe) {
    if (n < 3) seen[n] = cqe->user_data;
    n++;
  }
  check(n == 3 && seen[0] == 1 && seen[1] == 2 && seen[2] == 3,
        "for_each_cqe walks in order across the wrap");

  io_uring_cq_advance(&ring, 2);
  check(io_uring_cq_ready(&ring) == 1 && cq_head == 9,
        "a partial advance consumes exactly what it names");
  io_uring_for_each_cqe(&ring, head, cqe) {
    check(cqe->user_data == 3 && cqe->res == 33, "the walk resumes at the survivor");
    break;
  }

  /* The control-message walk, through whichever sys/socket.h this side
   * has - the shim's on Windows, the platform's elsewhere. */
  union {
    char bytes[64];
    struct cmsghdr align;
  } ctl;
  memset(&ctl, 0, sizeof(ctl));
  struct cmsghdr *c = (struct cmsghdr *) ctl.bytes;
  c->cmsg_len = CMSG_LEN(4);
  check(CMSG_DATA(c) == (unsigned char *) ctl.bytes + CMSG_ALIGN(sizeof(struct cmsghdr)),
        "CMSG_DATA lands right after the aligned header");

  printf(failed ? "liburing_h_run: FAILED\n" : "liburing_h_run: ok\n");
  return failed;
}
