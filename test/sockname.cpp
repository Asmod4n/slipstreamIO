// The three socket commands a reactor asks for by hand: the bound
// name of a port-0 listener, the peer's name at accept, and the
// socket's own memory accounting. All three ride IORING_OP_URING_CMD,
// and the first two have no liburing helper this header mirrors yet -
// they are spelled straight into the sqe, exactly as the consumer
// spells them (addr = the sockaddr, optval = the length in/out,
// optlen = 0 local / 1 peer).
#include <liburing.h>
#include <arpa/inet.h>
#include <linux/sock_diag.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

// One command, submitted and waited for on its own - the test asks a
// question at a time, so there is nothing to interleave.
static int cmd(struct io_uring* r) {
  io_uring_submit_and_wait(r, 1);
  struct io_uring_cqe* c;
  if (io_uring_peek_cqe(r, &c) != 0) return -EIO;
  const int res = c->res;
  io_uring_cqe_seen(r, c);
  return res;
}

int main() {
  alarm(60);
  struct io_uring r;
  if (io_uring_queue_init(64, &r, 0) != 0) return 1;
  io_uring_register_files_sparse(&r, 64);
  io_uring_register_file_alloc_range(&r, 0, 32);

  struct sockaddr_in a {};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = 0;  // the kernel picks; reading it back is the point
  struct io_uring_sqe* s = io_uring_get_sqe(&r);
  io_uring_prep_socket_direct(s, AF_INET, SOCK_STREAM, 0, 40, 0);
  s->flags |= IOSQE_IO_LINK;
  s = io_uring_get_sqe(&r);
  io_uring_prep_bind(s, 40, reinterpret_cast<struct sockaddr*>(&a), sizeof(a));
  s->flags |= IOSQE_FIXED_FILE | IOSQE_IO_LINK;
  s = io_uring_get_sqe(&r);
  io_uring_prep_listen(s, 40, 16);
  s->flags |= IOSQE_FIXED_FILE;
  io_uring_submit_and_wait(&r, 3);
  for (int i = 0; i < 3; i++) {
    struct io_uring_cqe* c;
    if (io_uring_peek_cqe(&r, &c) != 0) { std::printf("setup: missing cqe %d\n", i); return 2; }
    if (c->res < 0) { std::printf("setup %d: %s\n", i, std::strerror(-c->res)); return 2; }
    io_uring_cqe_seen(&r, c);
  }

  // GETSOCKNAME, local form: which port did the kernel pick?
  struct sockaddr_storage ss {};
  int slen = static_cast<int>(sizeof(ss));
  s = io_uring_get_sqe(&r);
  io_uring_prep_rw(IORING_OP_URING_CMD, s, 40, nullptr, 0, 0);
  s->cmd_op = SOCKET_URING_OP_GETSOCKNAME;
  s->addr = reinterpret_cast<uint64_t>(&ss);
  s->optval = reinterpret_cast<uint64_t>(&slen);
  s->optlen = 0;
  s->flags |= IOSQE_FIXED_FILE;
  int res = cmd(&r);
  if (res < 0) { std::printf("getsockname: %s\n", std::strerror(-res)); return 3; }
  if (ss.ss_family != AF_INET || slen < static_cast<int>(sizeof(struct sockaddr_in))) {
    std::printf("getsockname: family %d, len %d\n", (int)ss.ss_family, slen);
    return 3;
  }
  a.sin_port = reinterpret_cast<struct sockaddr_in*>(&ss)->sin_port;
  if (a.sin_port == 0) { std::printf("getsockname: port still 0\n"); return 3; }
  std::printf("getsockname (local): port 0 -> %u\n", ntohs(a.sin_port));

  pid_t kid = fork();
  if (kid == 0) {
    int c = socket(AF_INET, SOCK_STREAM, 0);
    for (int i = 0; i < 100 && connect(c, (struct sockaddr*)&a, sizeof(a)) != 0; i++) usleep(20000);
    char buf[64];
    ssize_t n = read(c, buf, sizeof(buf));
    _exit(n == 4 && memcmp(buf, "pong", 4) == 0 ? 0 : 3);
  }

  s = io_uring_get_sqe(&r);
  io_uring_prep_multishot_accept_direct(s, 40, nullptr, nullptr, 0);
  s->flags |= IOSQE_FIXED_FILE;
  res = cmd(&r);
  if (res < 0) { std::printf("accept: %s\n", std::strerror(-res)); return 4; }
  const int conn = res;

  // GETSOCKNAME, peer form: the address the access log spells as %h.
  struct sockaddr_storage ps {};
  int plen = static_cast<int>(sizeof(ps));
  s = io_uring_get_sqe(&r);
  io_uring_prep_rw(IORING_OP_URING_CMD, s, conn, nullptr, 0, 0);
  s->cmd_op = SOCKET_URING_OP_GETSOCKNAME;
  s->addr = reinterpret_cast<uint64_t>(&ps);
  s->optval = reinterpret_cast<uint64_t>(&plen);
  s->optlen = 1;
  s->flags |= IOSQE_FIXED_FILE;
  res = cmd(&r);
  if (res < 0) { std::printf("getpeername: %s\n", std::strerror(-res)); return 5; }
  char txt[INET6_ADDRSTRLEN] = {};
  if (ps.ss_family != AF_INET ||
      inet_ntop(AF_INET, &reinterpret_cast<struct sockaddr_in*>(&ps)->sin_addr, txt, sizeof txt) ==
          nullptr) {
    std::printf("getpeername: family %d, len %d\n", (int)ps.ss_family, plen);
    return 5;
  }
  if (std::strcmp(txt, "127.0.0.1") != 0) { std::printf("getpeername: %s?\n", txt); return 5; }
  std::printf("getsockname (peer): %s\n", txt);

  // GETSOCKOPT: SO_MEMINFO, the send room this socket has.
  uint32_t mi[SK_MEMINFO_VARS] = {};
  s = io_uring_get_sqe(&r);
  io_uring_prep_cmd_sock(s, SOCKET_URING_OP_GETSOCKOPT, conn, SOL_SOCKET, SO_MEMINFO, mi,
                         sizeof(mi));
  s->flags |= IOSQE_FIXED_FILE;
  res = cmd(&r);
  if (res < 0) { std::printf("SO_MEMINFO: %s\n", std::strerror(-res)); return 6; }
  if (res != static_cast<int>(sizeof(mi))) {
    std::printf("SO_MEMINFO: %d bytes, wanted %zu\n", res, sizeof(mi));
    return 6;
  }
  if (mi[SK_MEMINFO_SNDBUF] == 0) { std::printf("SO_MEMINFO: sndbuf 0?\n"); return 6; }
  std::printf("getsockopt SO_MEMINFO: %d bytes, sndbuf %u\n", res, mi[SK_MEMINFO_SNDBUF]);

  s = io_uring_get_sqe(&r);
  io_uring_prep_send(s, conn, "pong", 4, 0);
  s->flags |= IOSQE_FIXED_FILE;
  res = cmd(&r);
  if (res != 4) { std::printf("send: %d\n", res); return 7; }

  int st = 0;
  waitpid(kid, &st, 0);
  io_uring_queue_exit(&r);
  if (WEXITSTATUS(st) != 0) { std::printf("peer: exit %d\n", WEXITSTATUS(st)); return 8; }
  std::printf("socket commands: ok\n");
  return 0;
}
