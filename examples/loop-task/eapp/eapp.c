#include "app/eapp_utils.h"
#include <stdint.h>

unsigned long
main(void) {
  volatile uint64_t value = 1;

  for (;;) {
    value = value * 1664525ULL + 1013904223ULL;

    /* Prevent the compiler from optimizing away the busy loop. */
    __asm__ __volatile__("" : "+r"(value) : : "memory");
  }

  /* Unreachable. */
  EAPP_RETURN(0);
}
