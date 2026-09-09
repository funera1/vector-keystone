#include "keystone-sbi.h"

#define SBI_EXT_MIRALIS                    0x08475bcd
#define SBI_MIRALIS_REGISTER_PREEMPTION    5
#define SBI_MIRALIS_RESUME_PREEMPTED       6
#define SBI_MIRALIS_UNREGISTER_PREEMPTION  7

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

int keystone_register_preemption_target(unsigned long pc, unsigned long sp,
                                        unsigned long opaque)
{
  struct sbiret ret;

  ret = sbi_ecall(SBI_EXT_MIRALIS, SBI_MIRALIS_REGISTER_PREEMPTION,
                  pc, sp, opaque, 0, 0, 0);
  return ret.error;
}

void keystone_resume_preempted(unsigned long continuation_id)
{
  sbi_ecall(SBI_EXT_MIRALIS, SBI_MIRALIS_RESUME_PREEMPTED,
            continuation_id, 0, 0, 0, 0, 0);
  unreachable();
}

int keystone_unregister_preemption_target(void)
{
  struct sbiret ret;

  ret = sbi_ecall(SBI_EXT_MIRALIS, SBI_MIRALIS_UNREGISTER_PREEMPTION,
                  0, 0, 0, 0, 0, 0);
  return ret.error;
}
