#include "app/eapp_utils.h"

unsigned long main()
{
  unsigned long cycles;
  __asm__ __volatile__("rdtime %0" : "=r"(cycles));

  EAPP_RETURN(cycles);
}
