/* Windows has no sys/wait.h. liburing.h needs the types the waitid prep
 * names: idtype_t, id_t and siginfo_t - the last one only ever as a
 * pointer parked in an SQE, so a minimal struct carries it. */
#ifndef SLIPSTREAM_SHIM_SYS_WAIT_H
#define SLIPSTREAM_SHIM_SYS_WAIT_H

typedef enum {
  P_ALL,
  P_PID,
  P_PGID
} idtype_t;

typedef unsigned long id_t;

#ifndef SLIPSTREAM_SHIM_SIGINFO_T
#define SLIPSTREAM_SHIM_SIGINFO_T
typedef struct {
  int si_signo;
  int si_code;
  int si_errno;
} siginfo_t;
#endif

#endif
