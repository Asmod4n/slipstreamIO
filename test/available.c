/* The question, and the two things that must be true of the answer
 * whatever it is: it does not need liburing to ask, and it says the same
 * thing twice. */
#include "uring_available.h"

#include <stdio.h>

int main(void) {
  const int a = slipstream_uring_available();
  const int b = slipstream_uring_available();
  printf("io_uring usable for this process: %s\n", a ? "yes" : "no");
  if (a != b) {
    printf("asked twice, answered differently                    FAIL\n");
    return 1;
  }
  printf("asked twice, answered the same                       ok\n");
#ifdef __linux__
  printf("on linux, so the question was actually asked         ok\n");
#else
  printf("not linux, so the answer is compiled in              ok\n");
  if (a) { printf("but it said yes off linux                          FAIL\n"); return 1; }
#endif
  return 0;
}
