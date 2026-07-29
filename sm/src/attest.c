//******************************************************************************
// Copyright (c) 2018, The Regents of the University of California (Regents).
// All Rights Reserved. See LICENSE for license details.
//------------------------------------------------------------------------------
#include "enclave.h"
#include <crypto.h>
#include "page.h"
#include <sbi/sbi_console.h>

#define HASH_PAGES_PER_SLICE 16

unsigned long validate_and_hash_enclave(struct enclave* enclave){
  struct enclave_measurement *measurement = &enclave->measurement;
  unsigned int pages = 0;

  while (measurement->next_page < measurement->end_page &&
         pages < HASH_PAGES_PER_SLICE) {
    hash_extend_page(&measurement->ctx, (void*)measurement->next_page);
    measurement->next_page += RISCV_PGSIZE;
    pages++;
  }

  if (measurement->next_page < measurement->end_page)
    return SBI_ERR_SM_ENCLAVE_INTERRUPTED;

  hash_finalize(enclave->hash, &measurement->ctx);
  return SBI_ERR_SM_ENCLAVE_SUCCESS;
}
