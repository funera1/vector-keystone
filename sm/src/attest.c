//******************************************************************************
// Copyright (c) 2018, The Regents of the University of California (Regents).
// All Rights Reserved. See LICENSE for license details.
//------------------------------------------------------------------------------
#include "enclave.h"
#include <crypto.h>
#include "page.h"
#include <sbi/sbi_console.h>

unsigned long validate_and_hash_enclave(struct enclave* enclave){
  struct enclave_measurement *measurement = &enclave->measurement;
  while (measurement->next_page < measurement->end_page) {
    hash_extend_page(&measurement->ctx, (void*)measurement->next_page);
    measurement->next_page += RISCV_PGSIZE;
  }

  hash_finalize(enclave->hash, &measurement->ctx);
  return SBI_ERR_SM_ENCLAVE_SUCCESS;
}
