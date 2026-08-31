/* CMSG_ALIGN is the Linux/glibc spelling, and liburing.h's recvmsg
 * helpers walk control messages with it. FreeBSD's sys/socket.h does
 * not carry the name - found by the FreeBSD VM run, invisible on a
 * glibc host - and macOS does not either. Where it is missing it is
 * defined as the PLATFORM'S own control-message alignment, never an
 * invented one: the walk has to agree with the system's CMSG_SPACE and
 * CMSG_NXTHDR, and macOS aligns to 32 bits where the BSDs align to the
 * register size. */
#ifndef SLIPSTREAM_SHIM_POSIX_SYS_SOCKET_H
#define SLIPSTREAM_SHIM_POSIX_SYS_SOCKET_H

#include_next <sys/socket.h>

#ifndef CMSG_ALIGN
#if defined(__APPLE__)
#define CMSG_ALIGN(n) __DARWIN_ALIGN32(n)
#elif defined(__FreeBSD__) || defined(__DragonFly__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/param.h>
#define CMSG_ALIGN(n) _ALIGN(n)
#endif
#endif

#endif
