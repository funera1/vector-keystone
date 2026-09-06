#include <sbi/sbi_trap.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_tlb.h>
#include <sbi/sbi_ipi.h>
#include <sbi/sbi_string.h>
#include <sbi/riscv_locks.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_scratch.h>
#include <sbi/riscv_asm.h>
#include <sbi/sbi_ecall.h>
#include "sm-sbi-opensbi.h"
#include "pmp.h"
#include "sm-sbi.h"
#include "sm.h"
#include "cpu.h"

/*
 * Deliberately minimal M-mode preemption support for Miralis vM-mode.
 *
 * OpenSBI's timer trap calls keystone_mmode_timer_preempt() after acknowledging
 * MTIP.  When a Keystone SBI call is active, the hook abandons the nested
 * M-mode context and exits through the original S-mode trap frame.
 */
#define KEYSTONE_PREEMPT_MAX_HARTS 16
#define MIRALIS_EID 0x08475bcdUL
#define MIRALIS_PREEMPT_READY_FID 3UL

static struct sbi_trap_regs *volatile
  preempt_return_regs[KEYSTONE_PREEMPT_MAX_HARTS];

struct keystone_preempt_debug_state {
  unsigned long current_fid;
  unsigned long initial_a0;
  unsigned long lock_mask;
  unsigned long lock_site;
  unsigned long sequence;
};

static volatile struct keystone_preempt_debug_state
  preempt_debug[KEYSTONE_PREEMPT_MAX_HARTS];

void keystone_preempt_debug_lock_acquired(unsigned long lock,
                                          unsigned long site)
{
  unsigned long hartid = current_hartid();

  if (hartid >= KEYSTONE_PREEMPT_MAX_HARTS)
    return;

  preempt_debug[hartid].lock_mask |= lock;
  preempt_debug[hartid].lock_site = site;
}

void keystone_preempt_debug_lock_released(unsigned long lock)
{
  unsigned long hartid = current_hartid();

  if (hartid >= KEYSTONE_PREEMPT_MAX_HARTS)
    return;

  preempt_debug[hartid].lock_mask &= ~lock;
  if (!preempt_debug[hartid].lock_mask)
    preempt_debug[hartid].lock_site = 0;
}

static bool keystone_call_is_preemptible(unsigned long funcid)
{
  switch (funcid) {
    case SBI_SM_RUN_ENCLAVE:
    case SBI_SM_RESUME_ENCLAVE:
    case SBI_SM_STOP_ENCLAVE:
    case SBI_SM_EXIT_ENCLAVE:
      return false;
    default:
      return true;
  }
}

static unsigned long keystone_test_long_operation(void)
{
  volatile unsigned long progress;

  sbi_printf("[SM-TEST] long operation start\n");
  for (progress = 0; progress < 100000000; progress++)
    __asm__ __volatile__("" ::: "memory");
  sbi_printf("[SM-TEST] long operation complete progress=%lu\n", progress);

  return SBI_SUCCESS;
}

void keystone_mmode_timer_preempt(struct sbi_trap_regs *nested_regs)
{
  unsigned long hartid = current_hartid();
  struct sbi_trap_regs *return_regs;
  volatile struct keystone_preempt_debug_state *debug;

  if (hartid >= KEYSTONE_PREEMPT_MAX_HARTS ||
      (nested_regs->mstatus & MSTATUS_MPP) !=
        (PRV_M << MSTATUS_MPP_SHIFT))
    return;

  return_regs = preempt_return_regs[hartid];
  if (!return_regs)
    return;

#ifdef KEYSTONE_DEBUG_RANDOM_PREEMPT_ITERATIONS
  sbi_printf("[SM-DEBUG] machine timer handler hook hart=%lu interrupted_mepc=0x%lx interrupted_mstatus=0x%lx interrupted_ra=0x%lx fid=%lu\n",
             hartid, nested_regs->mepc, nested_regs->mstatus,
             nested_regs->ra, preempt_debug[hartid].current_fid);
#endif

  debug = &preempt_debug[hartid];
  debug->sequence++;

  /* Avoid printing here: the interrupted code may own OpenSBI's console lock. */
  register unsigned long a0 asm("a0") = debug->current_fid;
  register unsigned long a1 asm("a1") = nested_regs->mepc;
  register unsigned long a2 asm("a2") = nested_regs->ra;
  register unsigned long a3 asm("a3") = debug->lock_mask;
  register unsigned long a4 asm("a4") = debug->lock_site;
  register unsigned long a5 asm("a5") = return_regs->a0;
  register unsigned long a6 asm("a6") = MIRALIS_PREEMPT_READY_FID;
  register unsigned long a7 asm("a7") = MIRALIS_EID;
  asm volatile("ecall"
               : "+r"(a0), "+r"(a1), "+r"(a2), "+r"(a3),
                 "+r"(a4), "+r"(a5), "+r"(a6), "+r"(a7)
               :
               : "memory");
}

static int sbi_ecall_keystone_enclave_handler(unsigned long extid, unsigned long funcid,
                     const struct sbi_trap_regs *regs,
                     unsigned long *out_val,
                     struct sbi_trap_info *out_trap)
{
  uintptr_t retval;
  unsigned long hartid = current_hartid();
  bool preemptible = keystone_call_is_preemptible(funcid) &&
                     hartid < KEYSTONE_PREEMPT_MAX_HARTS;

  sbi_printf("[SM-FLOW] ecall enter hart=%lu fid=%lu enclave_ctx=%d mepc=0x%lx mstatus=0x%lx\n",
             hartid, funcid, cpu_is_enclave_context(), regs->mepc,
             regs->mstatus);

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

  if (preemptible) {
    if (funcid == SBI_SM_RANDOM)
      sbi_printf("[SM-DEBUG] random preempt setup begin hart=%lu mie=0x%lx mip=0x%lx mstatus=0x%lx\n",
                 hartid, csr_read(CSR_MIE), csr_read(CSR_MIP),
                 csr_read(CSR_MSTATUS));
    preempt_debug[hartid].current_fid = funcid;
    preempt_debug[hartid].initial_a0 = regs->a0;
    preempt_return_regs[hartid] = (struct sbi_trap_regs *)regs;
    csr_set(CSR_MIE, MIP_MTIP);
    if (funcid == SBI_SM_RANDOM)
      sbi_printf("[SM-DEBUG] random preempt MTIE enabled hart=%lu mie=0x%lx mip=0x%lx mstatus=0x%lx\n",
                 hartid, csr_read(CSR_MIE), csr_read(CSR_MIP),
                 csr_read(CSR_MSTATUS));
    if (funcid == SBI_SM_RANDOM)
      sbi_printf("[SM-DEBUG] random preempt enabling MIE hart=%lu\n", hartid);
    csr_set(CSR_MSTATUS, MSTATUS_MIE);
    if (funcid == SBI_SM_RANDOM)
      sbi_printf("[SM-DEBUG] random preempt MIE enabled hart=%lu mie=0x%lx mip=0x%lx mstatus=0x%lx\n",
                 hartid, csr_read(CSR_MIE), csr_read(CSR_MIP),
                 csr_read(CSR_MSTATUS));
  }

  switch (funcid) {
    case SBI_SM_TEST_LONG_OPERATION:
      retval = keystone_test_long_operation();
      break;
    case SBI_SM_CREATE_ENCLAVE:
      sbi_printf("[SM-DEBUG] create entry hart=%lu arg0=0x%lx initial_a0=0x%lx\n",
                 hartid, regs->a0, preempt_debug[hartid].initial_a0);
      retval = sbi_sm_create_enclave(out_val, regs->a0);
      break;
    case SBI_SM_RESUME_CREATE_ENCLAVE:
      retval = sbi_sm_resume_create_enclave(regs->a0);
      break;
    case SBI_SM_DESTROY_ENCLAVE:
      retval = sbi_sm_destroy_enclave(regs->a0);
      break;
    case SBI_SM_RUN_ENCLAVE:
      sbi_printf("[SM-FLOW] dispatch RUN_ENCLAVE hart=%lu eid=%lu\n",
                 hartid, regs->a0);
      retval = sbi_sm_run_enclave((struct sbi_trap_regs*) regs, regs->a0);
      __builtin_unreachable();
      break;
    case SBI_SM_RESUME_ENCLAVE:
      retval = sbi_sm_resume_enclave((struct sbi_trap_regs*) regs, regs->a0);
      __builtin_unreachable();
      break;
    case SBI_SM_RANDOM:
      sbi_printf("[SM-DEBUG] random dispatch hart=%lu\n", hartid);
      *out_val = sbi_sm_random();
      retval = 0;
      break;
    case SBI_SM_ATTEST_ENCLAVE:
      retval = sbi_sm_attest_enclave(regs->a0, regs->a1, regs->a2);
      break;
    case SBI_SM_GET_SEALING_KEY:
      retval = sbi_sm_get_sealing_key(regs->a0, regs->a1, regs->a2);
      break;
    case SBI_SM_STOP_ENCLAVE:
      retval = sbi_sm_stop_enclave((struct sbi_trap_regs*) regs, regs->a0);
      __builtin_unreachable();
      break;
    case SBI_SM_EXIT_ENCLAVE:
      retval = sbi_sm_exit_enclave((struct sbi_trap_regs*) regs, regs->a0);
      __builtin_unreachable();
      break;
    case SBI_SM_CALL_PLUGIN:
      retval = sbi_sm_call_plugin(regs->a0, regs->a1, regs->a2, regs->a3);
      break;
    default:
      retval = SBI_ERR_SM_NOT_IMPLEMENTED;
      break;
  }

  if (preemptible) {
    csr_clear(CSR_MSTATUS, MSTATUS_MIE);
    preempt_return_regs[hartid] = NULL;
    preempt_debug[hartid].current_fid = 0;
  }

  return retval;

}

struct sbi_ecall_extension ecall_keystone_enclave = {
  .extid_start = SBI_EXT_EXPERIMENTAL_KEYSTONE_ENCLAVE,
  .extid_end = SBI_EXT_EXPERIMENTAL_KEYSTONE_ENCLAVE,
  .handle = sbi_ecall_keystone_enclave_handler,
};
