#include "keystone-sbi.h"

#define SBI_EXT_MIRALIS                    0x08475bcd
#define SBI_MIRALIS_ACTIVATION_CALL       5
#define SBI_MIRALIS_ACTIVATION_RESUME     6

struct sbiret sbi_sm_create_enclave(struct keystone_sbi_create_t* args) {
  return sbi_ecall(SBI_EXT_EXPERIMENTAL_KEYSTONE_ENCLAVE,
      SBI_SM_CREATE_ENCLAVE,
      (unsigned long) args, 0, 0, 0, 0, 0);
}

struct sbiret sbi_sm_resume_create_enclave(unsigned long eid) {
  return sbi_ecall(SBI_EXT_EXPERIMENTAL_KEYSTONE_ENCLAVE,
      SBI_SM_RESUME_CREATE_ENCLAVE,
      eid, 0, 0, 0, 0, 0);
}

struct sbiret sbi_sm_run_enclave(unsigned long eid) {
  return sbi_ecall(SBI_EXT_EXPERIMENTAL_KEYSTONE_ENCLAVE,
      SBI_SM_RUN_ENCLAVE,
      eid, 0, 0, 0, 0, 0);
}

struct sbiret sbi_miralis_activation_call(unsigned long target_eid,
                                           unsigned long target_fid,
                                           unsigned long arg0,
                                           unsigned long arg1,
                                           unsigned long arg2,
                                           unsigned long arg3) {
  return sbi_ecall(SBI_EXT_MIRALIS, SBI_MIRALIS_ACTIVATION_CALL,
                   target_eid, target_fid, arg0, arg1, arg2, arg3);
}

struct sbiret sbi_miralis_activation_resume(unsigned long continuation_id) {
  return sbi_ecall(SBI_EXT_MIRALIS, SBI_MIRALIS_ACTIVATION_RESUME,
                   continuation_id, 0, 0, 0, 0, 0);
}

struct sbiret sbi_sm_destroy_enclave(unsigned long eid) {
  return sbi_ecall(SBI_EXT_EXPERIMENTAL_KEYSTONE_ENCLAVE,
      SBI_SM_DESTROY_ENCLAVE,
      eid, 0, 0, 0, 0, 0);
}

struct sbiret sbi_sm_resume_enclave(unsigned long eid) {
  return sbi_ecall(SBI_EXT_EXPERIMENTAL_KEYSTONE_ENCLAVE,
      SBI_SM_RESUME_ENCLAVE,
      eid, 0, 0, 0, 0, 0);
}
