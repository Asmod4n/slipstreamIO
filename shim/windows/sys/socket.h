/* Windows has no sys/socket.h. struct sockaddr and socklen_t come from
 * Winsock so a consumer that also includes it sees one definition, not
 * two. struct msghdr, struct cmsghdr and CMSG_ALIGN have no Winsock
 * counterpart (WSAMSG is a different layout) and liburing.h reads their
 * fields in the recvmsg helpers, so they are defined here, POSIX-shaped. */
#ifndef SLIPSTREAM_SHIM_SYS_SOCKET_H
#define SLIPSTREAM_SHIM_SYS_SOCKET_H

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stddef.h>
#include <sys/uio.h>

struct msghdr {
  void *msg_name;
  socklen_t msg_namelen;
  struct iovec *msg_iov;
  size_t msg_iovlen;
  void *msg_control;
  size_t msg_controllen;
  int msg_flags;
};

struct cmsghdr {
  size_t cmsg_len;
  int cmsg_level;
  int cmsg_type;
};

/* wincrypt.h (in via windows.h) owns these names for certificate
 * messages. Here they are the POSIX control-message macros, because
 * liburing.h's recvmsg helpers are written against those. */
#undef CMSG_ALIGN
#undef CMSG_SPACE
#undef CMSG_LEN
#undef CMSG_DATA
#define CMSG_ALIGN(len) (((len) + sizeof(size_t) - 1) & (size_t) ~(sizeof(size_t) - 1))
#define CMSG_SPACE(len) (CMSG_ALIGN(len) + CMSG_ALIGN(sizeof(struct cmsghdr)))
#define CMSG_LEN(len) (CMSG_ALIGN(sizeof(struct cmsghdr)) + (len))
#define CMSG_DATA(cmsg) ((unsigned char *) (cmsg) + CMSG_ALIGN(sizeof(struct cmsghdr)))

#endif
