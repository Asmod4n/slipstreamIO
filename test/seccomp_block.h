#ifndef SLIPSTREAM_TEST_SECCOMP_BLOCK_H
#define SLIPSTREAM_TEST_SECCOMP_BLOCK_H

/* What a container runtime does to io_uring, done to ourselves: the
 * three io_uring syscalls answer EPERM, everything else runs. Installing
 * it needs no privilege beyond no_new_privs, and it cannot be taken off
 * again for the life of the process - which is also true in the real
 * case, and is why each test that wants it is its own process.
 *
 * Test harness only; nothing in src/ includes this. */

#include <errno.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

static int block_io_uring_syscalls(void) {
  struct sock_filter f[] = {
    BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_io_uring_setup, 2, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_io_uring_enter, 1, 0),
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_io_uring_register, 0, 1),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  };
  struct sock_fprog prog = { .len = sizeof(f) / sizeof(f[0]), .filter = f };
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return -1;
  if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) != 0) return -1;
  return 0;
}

#endif
