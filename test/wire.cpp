// A real connection driven only through slipstreamIO: the exact op set
// a reactor uses - socket_direct, bind, listen linked in one chain,
// multishot accept, multishot recv off a provided-buffer ring, send.
#include <slipstreamio.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

static const int kPort = 28731;

int main() {
  struct io_uring r;
  if (io_uring_queue_init(64, &r, 0) != 0) return 1;
  io_uring_register_files_sparse(&r, 64);
  io_uring_register_file_alloc_range(&r, 0, 32);

  static char pool[8][1024];
  int e = 0;
  struct io_uring_buf_ring* br = io_uring_setup_buf_ring(&r, 8, 0, 0, &e);
  const int mask = io_uring_buf_ring_mask(8);
  for (int i = 0; i < 8; i++) io_uring_buf_ring_add(br, pool[i], 1024, i, mask, i);
  io_uring_buf_ring_advance(br, 8);

  struct sockaddr_in a {};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(kPort);
  struct io_uring_sqe* s = io_uring_get_sqe(&r);
  io_uring_prep_socket_direct(s, AF_INET, SOCK_STREAM, 0, 40, 0);
  s->flags |= IOSQE_IO_LINK;
  int one = 1;
  s = io_uring_get_sqe(&r);
  io_uring_prep_cmd_sock(s, SOCKET_URING_OP_SETSOCKOPT, 40, SOL_SOCKET, SO_REUSEADDR, &one,
                         sizeof(one));
  s->flags |= IOSQE_FIXED_FILE | IOSQE_IO_LINK;
  s = io_uring_get_sqe(&r);
  io_uring_prep_bind(s, 40, reinterpret_cast<struct sockaddr*>(&a), sizeof(a));
  s->flags |= IOSQE_FIXED_FILE | IOSQE_IO_LINK;
  s = io_uring_get_sqe(&r);
  io_uring_prep_listen(s, 40, 16);
  s->flags |= IOSQE_FIXED_FILE;
  io_uring_submit(&r);
  for (int i = 0; i < 4; i++) {
    struct io_uring_cqe* c;
    if (io_uring_peek_cqe(&r, &c) != 0) { std::printf("setup: missing cqe %d\n", i); return 2; }
    if (c->res < 0) { std::printf("setup %d: %s\n", i, std::strerror(-c->res)); return 2; }
    io_uring_cqe_seen(&r, c);
  }
  std::printf("socket+setsockopt+bind+listen, one linked chain: ok\n");

  pid_t kid = fork();
  if (kid == 0) {  // the peer, plain sockets
    int c = socket(AF_INET, SOCK_STREAM, 0);
    for (int i = 0; i < 100 && connect(c, (struct sockaddr*)&a, sizeof(a)) != 0; i++) usleep(20000);
    if (write(c, "ping", 4) != 4) _exit(9);
    char buf[64];
    ssize_t n = read(c, buf, sizeof(buf));
    _exit(n == 4 && memcmp(buf, "pong", 4) == 0 ? 0 : 3);
  }

  s = io_uring_get_sqe(&r);
  io_uring_prep_multishot_accept_direct(s, 40, nullptr, nullptr, 0);
  s->flags |= IOSQE_FIXED_FILE;
  io_uring_sqe_set_data64(s, 1);

  int conn = -1;
  bool done = false;
  while (!done) {
    io_uring_submit_and_wait(&r, 1);
    struct io_uring_cqe* c;
    while (io_uring_peek_cqe(&r, &c) == 0) {
      const uint64_t tag = io_uring_cqe_get_data64(c);
      const int res = c->res;
      const uint32_t fl = c->flags;
      io_uring_cqe_seen(&r, c);
      if (tag == 1) {
        if (res < 0) { std::printf("accept: %s\n", std::strerror(-res)); return 3; }
        conn = res;
        std::printf("multishot accept -> direct slot %d\n", conn);
        s = io_uring_get_sqe(&r);
        io_uring_prep_recv_multishot(s, conn, nullptr, 0, 0);
        s->flags |= IOSQE_FIXED_FILE | IOSQE_BUFFER_SELECT;
        s->buf_group = 0;
        io_uring_sqe_set_data64(s, 2);
      } else if (tag == 2) {
        if (res <= 0) { std::printf("recv: %d\n", res); return 4; }
        if (!(fl & IORING_CQE_F_BUFFER)) { std::printf("recv: no buffer id\n"); return 4; }
        const unsigned bid = fl >> IORING_CQE_BUFFER_SHIFT;
        std::printf("multishot recv: %d bytes in buffer %u = \"%.*s\"\n", res, bid, res, pool[bid]);
        if (std::string(pool[bid], res) != "ping") return 5;
        s = io_uring_get_sqe(&r);
        io_uring_prep_send(s, conn, "pong", 4, 0);
        s->flags |= IOSQE_FIXED_FILE;
        io_uring_sqe_set_data64(s, 3);
      } else if (tag == 3) {
        if (res != 4) { std::printf("send: %d\n", res); return 6; }
        std::printf("send: 4 bytes\n");
        done = true;
      }
    }
  }
  int st = 0;
  waitpid(kid, &st, 0);
  io_uring_queue_exit(&r);
  std::printf("peer saw the reply: %s\n", WEXITSTATUS(st) == 0 ? "yes" : "NO");
  return WEXITSTATUS(st);
}
