//******************************************************************************
// Copyright (c) 2018, The Regents of the University of California (Regents).
// All Rights Reserved. See LICENSE for license details.
//------------------------------------------------------------------------------
#include "edge/edge_call.h"
#include "host/keystone.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <unistd.h>

using namespace Keystone;

static unsigned long long
monotonic_ns() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return static_cast<unsigned long long>(ts.tv_sec) * 1000000000ULL +
         static_cast<unsigned long long>(ts.tv_nsec);
}

static void
phase_marker(const char* phase) {
  fprintf(stderr, "PHASE,%s,%llu\n", phase, monotonic_ns());
  fflush(stderr);
}

static void
phase_pause() {
  const char* value = getenv("KEYSTONE_PHASE_DELAY_SECONDS");
  unsigned int seconds = value ? static_cast<unsigned int>(strtoul(value, nullptr, 10)) : 2;
  sleep(seconds);
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

  phase_marker("experiment_start");
  phase_pause();

  {
    phase_marker("object_create_start");
    Enclave enclave;
    phase_marker("object_create_end");
    phase_pause();

    phase_marker("init_start");
    enclave.init(argv[1], argv[2], argv[3], params);
    phase_marker("init_end");
    phase_pause();

    phase_marker("ocall_setup_start");
    enclave.registerOcallDispatch(incoming_call_dispatch);
    edge_call_init_internals(
        (uintptr_t)enclave.getSharedBuffer(), enclave.getSharedBufferSize());
    phase_marker("ocall_setup_end");
    phase_pause();

    phase_marker("run_start");
    enclave.run();
    phase_marker("run_end");
    phase_pause();

    phase_marker("destroy_start");
  }

  phase_marker("destroy_end");
  phase_marker("experiment_end");

  return 0;
}
