#ifndef _SM_SBI_OPENSBI_H_
#define _SM_SBI_OPENSBI_H_

#define SBI_SM_EVENT 0x0100
#define KEYSTONE_MM_PREEMPT_VERSION "miralis-mmode-preempt-v1"
#define KEYSTONE_DEBUG_LOCK_ENCLAVE (1UL << 0)
#define KEYSTONE_DEBUG_LOCK_PMP     (1UL << 1)
#include "sbi/sbi_trap.h"
#include "sbi/sbi_error.h"
#include "sbi/sbi_scratch.h"
#include <sbi/sbi_ecall.h>

#include "sm_call.h"

/* Inbound interfaces */
extern struct sbi_ecall_extension ecall_keystone_enclave;

/* Called by OpenSBI after processing an M-mode timer interrupt. */
void keystone_mmode_timer_preempt(struct sbi_trap_regs *regs);

/* Record lock ownership without printing from interrupt context. */
void keystone_preempt_debug_lock_acquired(unsigned long lock,
                                          unsigned long site);
void keystone_preempt_debug_lock_released(unsigned long lock);

//int sbi_sm_interface(struct sbi_scratch *scratch, unsigned long extension_id,
//                     struct sbi_trap_regs  *regs,
//                     unsigned long *out_val,
//                     struct sbi_trap_info *out_trap);
//void sm_ipi_process();

/* Outbound interfaces */
//int sm_sbi_send_ipi(uintptr_t recipient_mask);
#endif /*_SM_SBI_OPENSBI_H_*/
