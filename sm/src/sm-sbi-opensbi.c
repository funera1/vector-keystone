#include <sbi/sbi_trap.h>
#include <sbi/sbi_error.h>
#include "sm-sbi-opensbi.h"
#include "sm-sbi.h"
#include "sm.h"
#include "cpu.h"

static unsigned long keystone_test_long_operation(void)
{
  volatile unsigned long progress;

  for (progress = 0; progress < 100000000; progress++)
    __asm__ __volatile__("" ::: "memory");

  return SBI_SUCCESS;
}

static int sbi_ecall_keystone_enclave_handler(unsigned long extid, unsigned long funcid,
                     const struct sbi_trap_regs *regs,
                     unsigned long *out_val,
                     struct sbi_trap_info *out_trap)
{
  uintptr_t retval;

  if (funcid <= FID_RANGE_DEPRECATED) { return SBI_ERR_SM_DEPRECATED; }
  else if (funcid <= FID_RANGE_HOST)
  {
    if (cpu_is_enclave_context())
      return SBI_ERR_SM_ENCLAVE_SBI_PROHIBITED;
  }
  else if (funcid <= FID_RANGE_ENCLAVE)
  {
    if (!cpu_is_enclave_context())
      return SBI_ERR_SM_ENCLAVE_SBI_PROHIBITED;
  }

  switch (funcid) {
    case SBI_SM_TEST_LONG_OPERATION:
      sbi_sm_complete_host_call((struct sbi_trap_regs *)regs,
                               keystone_test_long_operation(), 0);
    case SBI_SM_CREATE_ENCLAVE:
      *out_val = 0;
      retval = sbi_sm_create_enclave(out_val, regs->a0);
      sbi_sm_complete_host_call((struct sbi_trap_regs *)regs, retval, *out_val);
    case SBI_SM_DESTROY_ENCLAVE:
      retval = sbi_sm_destroy_enclave(regs->a0);
      sbi_sm_complete_host_call((struct sbi_trap_regs *)regs, retval, 0);
    case SBI_SM_RUN_ENCLAVE:
      return sbi_sm_run_enclave((struct sbi_trap_regs *)regs, regs->a0);
    case SBI_SM_RESUME_ENCLAVE:
      return sbi_sm_resume_enclave((struct sbi_trap_regs *)regs, regs->a0);
    case SBI_SM_RANDOM:
      *out_val = sbi_sm_random();
      return 0;
    case SBI_SM_ATTEST_ENCLAVE:
      return sbi_sm_attest_enclave(regs->a0, regs->a1, regs->a2);
    case SBI_SM_GET_SEALING_KEY:
      return sbi_sm_get_sealing_key(regs->a0, regs->a1, regs->a2);
    case SBI_SM_STOP_ENCLAVE:
      return sbi_sm_stop_enclave((struct sbi_trap_regs *)regs, regs->a0);
    case SBI_SM_EXIT_ENCLAVE:
      return sbi_sm_exit_enclave((struct sbi_trap_regs *)regs, regs->a0);
    case SBI_SM_CALL_PLUGIN:
      return sbi_sm_call_plugin(regs->a0, regs->a1, regs->a2, regs->a3);
    default:
      return SBI_ERR_SM_NOT_IMPLEMENTED;
  }
}

struct sbi_ecall_extension ecall_keystone_enclave = {
  .extid_start = SBI_EXT_EXPERIMENTAL_KEYSTONE_ENCLAVE,
  .extid_end = SBI_EXT_EXPERIMENTAL_KEYSTONE_ENCLAVE,
  .handle = sbi_ecall_keystone_enclave_handler,
};
