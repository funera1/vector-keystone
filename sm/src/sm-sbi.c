//******************************************************************************
// Copyright (c) 2018, The Regents of the University of California (Regents).
// All Rights Reserved. See LICENSE for license details.
//------------------------------------------------------------------------------
#include "sm-sbi.h"
#include "pmp.h"
#include "enclave.h"
#include "page.h"
#include "cpu.h"
#include "platform-hook.h"
#include "plugins/plugins.h"
#include <sbi/riscv_asm.h>
#include <sbi/sbi_console.h>

#define SBI_EXT_MIRALIS 0x08475bcdUL
#define MIRALIS_ACTIVATION_COMPLETE 7UL

static void miralis_activation_complete(struct sbi_trap_regs *regs,
                                         unsigned long error,
                                         unsigned long value)
{
  /* The host frame is restored by exit_enclave/stop_enclave.  Its mepc still
   * points at the original ECALL: sbi_trap_exit does not advance it.  Execute
   * that instruction with the generic completion ABI instead. */
  regs->a0 = error;
  regs->a1 = value;
  regs->a6 = MIRALIS_ACTIVATION_COMPLETE;
  regs->a7 = SBI_EXT_MIRALIS;
}

unsigned long sbi_sm_create_enclave(unsigned long* eid, uintptr_t create_args)
{
  struct keystone_sbi_create_t create_args_local;
  unsigned long ret;

  ret = copy_enclave_create_args(create_args, &create_args_local);
  sbi_printf("[SM-FLOW] create args copied hart=%u ret=0x%lx\n",
             current_hartid(), ret);

  if (ret)
    return ret;

  sbi_printf("[SM-FLOW] create enclave begin hart=%u epm=0x%lx+0x%lx utm=0x%lx+0x%lx\n",
             current_hartid(), create_args_local.epm_region.paddr,
             create_args_local.epm_region.size, create_args_local.utm_region.paddr,
             create_args_local.utm_region.size);
  ret = create_enclave(eid, create_args_local);
  sbi_printf("[SM-FLOW] create enclave complete hart=%u ret=0x%lx\n",
             current_hartid(), ret);
  return ret;
}

unsigned long sbi_sm_resume_create_enclave(unsigned long eid)
{
  return resume_create_enclave((enclave_id)eid);
}

unsigned long sbi_sm_destroy_enclave(unsigned long eid)
{
  unsigned long ret;
  ret = destroy_enclave((unsigned int)eid);
  return ret;
}

unsigned long sbi_sm_run_enclave(struct sbi_trap_regs *regs, unsigned long eid)
{
  sbi_printf("[SM-FLOW] sbi_sm_run_enclave enter hart=%u eid=%lu mepc=0x%lx mstatus=0x%lx\n",
             current_hartid(), eid, regs->mepc, regs->mstatus);
  regs->a0 = run_enclave(regs, (unsigned int) eid);
  sbi_printf("[SM-FLOW] sbi_sm_run_enclave before trap exit hart=%u eid=%lu ret=0x%lx mepc=0x%lx mstatus=0x%lx\n",
             current_hartid(), eid, regs->a0, regs->mepc,
             regs->mstatus);
  regs->mepc += 4;
  sbi_printf("[SM-FLOW] sbi_sm_run_enclave trap exit hart=%u eid=%lu mepc=0x%lx mstatus=0x%lx\n",
             current_hartid(), eid, regs->mepc, regs->mstatus);
  sbi_trap_exit(regs);
  return 0;
}

unsigned long sbi_sm_resume_enclave(struct sbi_trap_regs *regs, unsigned long eid)
{
  unsigned long ret;
  ret = resume_enclave(regs, (unsigned int) eid);
  if (!regs->zero)
    regs->a0 = ret;
  regs->mepc += 4;

  sbi_trap_exit(regs);
  return 0;
}

unsigned long sbi_sm_exit_enclave(struct sbi_trap_regs *regs, unsigned long retval)
{
  regs->a0 = exit_enclave(regs, cpu_get_enclave_id());
  regs->a1 = retval;
  miralis_activation_complete(regs, regs->a0, regs->a1);
  sbi_trap_exit(regs);
  return 0;
}

unsigned long sbi_sm_stop_enclave(struct sbi_trap_regs *regs, unsigned long request)
{
  regs->a0 = stop_enclave(regs, request, cpu_get_enclave_id());
  miralis_activation_complete(regs, regs->a0, 0);
  sbi_trap_exit(regs);
  return 0;
}

unsigned long sbi_sm_attest_enclave(uintptr_t report, uintptr_t data, uintptr_t size)
{
  unsigned long ret;
  ret = attest_enclave(report, data, size, cpu_get_enclave_id());
  return ret;
}

unsigned long sbi_sm_get_sealing_key(uintptr_t sealing_key, uintptr_t key_ident,
                       size_t key_ident_size)
{
  unsigned long ret;
  ret = get_sealing_key(sealing_key, key_ident, key_ident_size,
                         cpu_get_enclave_id());
  return ret;
}

unsigned long sbi_sm_random(void)
{
#ifdef KEYSTONE_DEBUG_RANDOM_PREEMPT_ITERATIONS
  unsigned long random = 0;
  volatile unsigned long iteration;

  sbi_printf("[SM-DEBUG] random preemption window start iterations=%lu\n",
             (unsigned long) KEYSTONE_DEBUG_RANDOM_PREEMPT_ITERATIONS);
  for (iteration = 0;
       iteration < KEYSTONE_DEBUG_RANDOM_PREEMPT_ITERATIONS;
       iteration++)
    random = (unsigned long) platform_random();
  sbi_printf("[SM-DEBUG] random preemption window complete\n");
  return random;
#else
  return (unsigned long) platform_random();
#endif
}

unsigned long sbi_sm_call_plugin(uintptr_t plugin_id, uintptr_t call_id, uintptr_t arg0, uintptr_t arg1)
{
  unsigned long ret;
  ret = call_plugin(cpu_get_enclave_id(), plugin_id, call_id, arg0, arg1);
  return ret;
}
