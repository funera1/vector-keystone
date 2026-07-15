//******************************************************************************
// Copyright (c) 2018, The Regents of the University of California (Regents).
// All Rights Reserved. See LICENSE for license details.
//------------------------------------------------------------------------------
#define _GNU_SOURCE

#include "edge/edge_call.h"
#include "host/keystone.h"
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>

using namespace Keystone;

static int
pin_to_cpu(int cpu) {
  cpu_set_t set;

  CPU_ZERO(&set);
  CPU_SET(cpu, &set);

  if (sched_setaffinity(0, sizeof(set), &set) != 0) {
    perror("sched_setaffinity");
    return -1;
  }

  return 0;
}

int
main(int argc, char** argv) {
  int cpu = 2;

  // argv[1..3] are the eapp, runtime, and loader paths embedded by makeself.
  if (argc >= 5) {
    cpu = atoi(argv[4]);
  }

  if (cpu < 0) {
    fprintf(stderr, "Invalid CPU: %d\n", cpu);
    return 1;
  }

  if (pin_to_cpu(cpu) != 0) {
    return 1;
  }

  printf("Keystone host pinned to CPU %d\n", cpu);

  Enclave enclave;
  Params params;

  params.setFreeMemSize(256 * 1024);
  params.setUntrustedSize(256 * 1024);
  enclave.init(argv[1], argv[2], argv[3], params);

  enclave.registerOcallDispatch(incoming_call_dispatch);

  edge_call_init_internals(
      (uintptr_t)enclave.getSharedBuffer(), enclave.getSharedBufferSize());

  enclave.run();

  return 0;
}
