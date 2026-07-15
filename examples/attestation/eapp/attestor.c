//******************************************************************************
// Copyright (c) 2018, The Regents of the University of California (Regents).
// All Rights Reserved. See LICENSE for license details.
//------------------------------------------------------------------------------
#include "app/eapp_utils.h"
#include "app/syscall.h"
#include "edge/edge_common.h"

#define OCALL_PRINT_BUFFER 1
#define OCALL_PRINT_VALUE 2
#define OCALL_COPY_REPORT 3
#define OCALL_GET_STRING 4
#define OCALL_PHASE_MARKER 5

static void
phase_marker(const char* phase, size_t size) {
  ocall(OCALL_PHASE_MARKER, (void*)phase, size, 0, 0);
}

int
main() {
  struct edge_data retdata;
  ocall(OCALL_GET_STRING, NULL, 0, &retdata, sizeof(struct edge_data));

  for (unsigned long i = 1; i <= 10000; i++) {
    if (i % 5000 == 0) {
      ocall(OCALL_PRINT_VALUE, &i, sizeof(unsigned long), 0, 0);
    }
  }

  char nonce[2048];
  if (retdata.size > 2048) retdata.size = 2048;
  copy_from_shared(nonce, retdata.offset, retdata.size);

  char buffer[2048];
  phase_marker("attest_start", sizeof("attest_start"));
  attest_enclave((void*)buffer, nonce, retdata.size);
  phase_marker("attest_end", sizeof("attest_end"));

  phase_marker("report_copy_start", sizeof("report_copy_start"));
  ocall(OCALL_COPY_REPORT, buffer, 2048, 0, 0);
  phase_marker("report_copy_end", sizeof("report_copy_end"));

  EAPP_RETURN(0);
}
