#include "edge/edge_call.h"
#include "host/keystone.h"
#include <stdint.h>
#include <stdio.h>
#include <time.h>

using namespace Keystone;

// This workload targets the VisionFive2/JH7110 4 MHz timebase.
#define CLOCK_FREQ 4000000UL
#define NSEC_PER_SEC 1000000000L
#define USEC_PER_SEC 1000000L

static struct timespec
to_timespec(unsigned long ticks) {
  struct timespec time;
  time.tv_sec = ticks / CLOCK_FREQ;
  time.tv_nsec = (ticks % CLOCK_FREQ) * (NSEC_PER_SEC / CLOCK_FREQ);
  return time;
}

static int64_t
elapsed_us(struct timespec end, struct timespec start) {
  return USEC_PER_SEC * (int64_t)(end.tv_sec - start.tv_sec) +
         (end.tv_nsec - start.tv_nsec) / 1000;
}

int
main(int argc, char** argv) {
  if (argc < 4) {
    fprintf(stderr, "Usage: %s <eapp> <runtime> <loader>\n", argv[0]);
    return 1;
  }

  Params params;
  params.setFreeMemSize(256 * 1024);
  params.setUntrustedSize(256 * 1024);

  for (int i = 0; i < 10; i++) {
    unsigned long before;
    uintptr_t enclave_time;
    __asm__ __volatile__("rdtime %0" : "=r"(before));

    Enclave enclave;
    if (enclave.init(argv[1], argv[2], argv[3], params) != Error::Success)
      return 1;

    enclave.registerOcallDispatch(incoming_call_dispatch);
    edge_call_init_internals(
        (uintptr_t)enclave.getSharedBuffer(), enclave.getSharedBufferSize());
    if (enclave.run(&enclave_time) != Error::Success)
      return 1;

    printf("\t%lld\n", (long long)elapsed_us(
        to_timespec(enclave_time), to_timespec(before)));
  }

  return 0;
}
