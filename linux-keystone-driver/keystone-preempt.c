#include "keystone.h"
#include "keystone-sbi.h"

#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/smp.h>

#define MIRALIS_PREEMPTION_REASON_TIMER 1
#define MIRALIS_PREEMPT_STACK_SIZE       THREAD_SIZE

struct keystone_preempt_cpu {
  unsigned long continuation_id;
  unsigned char stack[MIRALIS_PREEMPT_STACK_SIZE] __aligned(16);
};

static DEFINE_PER_CPU(struct keystone_preempt_cpu, keystone_preempt_cpus);

/*
 * Miralis enters this function directly from virtual firmware, rather than through the normal
 * Linux trap entry.  It must never return: the matching firmware continuation is restored by
 * the RESUME_PREEMPTED SBI call below.
 */
static noinline __noreturn void keystone_preempt_trampoline(unsigned long reason,
                                                              unsigned long continuation_id,
                                                              unsigned long opaque)
{
  struct keystone_preempt_cpu *cpu = this_cpu_ptr(&keystone_preempt_cpus);

  if (reason != MIRALIS_PREEMPTION_REASON_TIMER)
    panic("keystone_enclave: unknown Miralis preemption reason %lu", reason);

  cpu->continuation_id = continuation_id;
  keystone_info("preemption trampoline: cpu=%u continuation=%lu opaque=0x%lx\n",
                smp_processor_id(), continuation_id, opaque);

  /* A continuation belongs to this hart's Miralis state. */
  migrate_disable();
  schedule();
  continuation_id = cpu->continuation_id;
  migrate_enable();

  keystone_info("preemption trampoline: resuming cpu=%u continuation=%lu\n",
                smp_processor_id(), continuation_id);
  keystone_resume_preempted(continuation_id);
}

static void keystone_register_preemption_target_cpu(void *unused)
{
  struct keystone_preempt_cpu *cpu = this_cpu_ptr(&keystone_preempt_cpus);
  unsigned long sp = (unsigned long)cpu->stack + sizeof(cpu->stack);
  int ret;

  ret = keystone_register_preemption_target(
      (unsigned long)keystone_preempt_trampoline, sp, (unsigned long)cpu);
  if (ret)
    keystone_err("failed to register preemption target on cpu=%u: %d\n",
                 smp_processor_id(), ret);
}

static void keystone_unregister_preemption_target_cpu(void *unused)
{
  int ret = keystone_unregister_preemption_target();

  if (ret)
    keystone_err("failed to unregister preemption target on cpu=%u: %d\n",
                 smp_processor_id(), ret);
}

void keystone_preemption_init(void)
{
  on_each_cpu(keystone_register_preemption_target_cpu, NULL, 1);
}

void keystone_preemption_exit(void)
{
  on_each_cpu(keystone_unregister_preemption_target_cpu, NULL, 1);
}
