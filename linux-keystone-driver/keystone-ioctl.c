//******************************************************************************
// Copyright (c) 2018, The Regents of the University of California (Regents).
// All Rights Reserved. See LICENSE for license details.
//------------------------------------------------------------------------------
#include "keystone.h"
#include "keystone-sbi.h"
#include "keystone_user.h"
#include <asm/csr.h>
#include <asm/sbi.h>
#include <linux/irqflags.h>
#include <linux/preempt.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/sched.h>
#include <asm/tlbflush.h>

static int keystone_create_enclave(struct file *filep, unsigned long arg)
{
  /* create parameters */
  struct keystone_ioctl_create_enclave *enclp = (struct keystone_ioctl_create_enclave *) arg;

  struct enclave *enclave;
  enclave = create_enclave(enclp->min_pages);

  if (enclave == NULL) {
    return -ENOMEM;
  }

  /* Pass base page table */
  enclp->epm_paddr = enclave->epm->pa;
  enclp->epm_size = enclave->epm->size;

  /* allocate UID */
  enclp->eid = enclave_idr_alloc(enclave);

  filep->private_data = (void *) enclp->eid;

  return 0;
}


static int keystone_finalize_enclave(unsigned long arg)
{
  struct sbiret ret;
  unsigned long resume_count = 0;
  struct enclave *enclave;
  struct utm *utm;
  struct keystone_sbi_create_t create_args;

  struct keystone_ioctl_create_enclave *enclp = (struct keystone_ioctl_create_enclave *) arg;

  enclave = get_enclave_by_id(enclp->eid);
  if(!enclave) {
    keystone_err("invalid enclave id\n");
    return -EINVAL;
  }

  enclave->is_init = false;

  /* SBI Call */
  create_args.epm_region.paddr = enclave->epm->pa;
  create_args.epm_region.size = enclave->epm->size;

  utm = enclave->utm;

  if (utm) {
    create_args.utm_region.paddr = __pa(utm->ptr);
    create_args.utm_region.size = utm->size;
  } else {
    create_args.utm_region.paddr = 0;
    create_args.utm_region.size = 0;
  }

  // physical addresses for runtime, user, and freemem
  create_args.runtime_paddr = enclp->runtime_paddr;
  create_args.user_paddr = enclp->user_paddr;
  create_args.free_paddr = enclp->free_paddr;
  create_args.free_requested = enclp->free_requested;

  ret = sbi_sm_create_enclave(&create_args);
  keystone_info("finalize: CREATE_ENCLAVE returned error=0x%lx value=0x%lx\n",
                (unsigned long)ret.error, (unsigned long)ret.value);

  if (ret.error == SBI_ERR_SM_ENCLAVE_INTERRUPTED) {
    enclave->eid = ret.value;
    do {
      cond_resched();
      ret = sbi_sm_resume_create_enclave(enclave->eid);
      resume_count++;
      if (resume_count <= 8 || !(resume_count & (resume_count - 1)) ||
          ret.error != SBI_ERR_SM_ENCLAVE_INTERRUPTED)
        keystone_info("finalize: RESUME_CREATE_ENCLAVE count=%lu eid=%lu "
                      "error=0x%lx value=0x%lx\n",
                      resume_count, enclave->eid, (unsigned long)ret.error,
                      (unsigned long)ret.value);
    } while (ret.error == SBI_ERR_SM_ENCLAVE_INTERRUPTED);
  }

  keystone_info("finalize: resume loop exited count=%lu error=0x%lx value=0x%lx\n",
                resume_count, (unsigned long)ret.error,
                (unsigned long)ret.value);

  if (ret.error) {
    keystone_err("keystone_create_enclave: SBI call failed with error code %ld\n", ret.error);
    goto error_destroy_enclave;
  }

  if (enclave->eid == KEYSTONE_INVALID_EID)
    enclave->eid = ret.value;

  keystone_info("finalize: returning success eid=%lu\n", enclave->eid);
  return 0;

error_destroy_enclave:
  /* This can handle partial initialization failure */
  if (enclave->eid != KEYSTONE_INVALID_EID) {
    struct sbiret destroy_ret = sbi_sm_destroy_enclave(enclave->eid);
    if (destroy_ret.error)
      keystone_err("failed to destroy partially initialized enclave: SBI error %ld\n",
                   destroy_ret.error);
    enclave->eid = KEYSTONE_INVALID_EID;
  }
  destroy_enclave(enclave);

  return -EINVAL;

}

static int keystone_run_enclave(unsigned long data)
{
  struct sbiret ret;
  unsigned long ueid;
  struct enclave* enclave;
  struct keystone_ioctl_run_enclave *arg = (struct keystone_ioctl_run_enclave*) data;

  ueid = arg->eid;
  enclave = get_enclave_by_id(ueid);

  if (!enclave) {
    keystone_err("invalid enclave id\n");
    return -EINVAL;
  }

  if (enclave->eid == KEYSTONE_INVALID_EID) {
    keystone_err("real enclave does not exist\n");
    return -EINVAL;
  }

  pr_info("keystone_enclave: RUN: before RUN_ENCLAVE eid=%lu\n", enclave->eid);
  ret = sbi_miralis_activation_call(
      SBI_EXT_EXPERIMENTAL_KEYSTONE_ENCLAVE, SBI_SM_RUN_ENCLAVE,
      enclave->eid, 0, 0, 0);
  while (ret.error == (long)-9) {
    cond_resched();
    ret = sbi_miralis_activation_resume(ret.value);
  }
  pr_info("keystone_enclave: RUN: after RUN_ENCLAVE eid=%lu error=0x%lx value=0x%lx\n",
          enclave->eid, ret.error, ret.value);

  arg->error = ret.error;
  arg->value = ret.value;

  return 0;
}

static int utm_init_ioctl(struct file *filp, unsigned long arg)
{
  int ret = 0;
  struct utm *utm;
  struct enclave *enclave;
  struct keystone_ioctl_create_enclave *enclp = (struct keystone_ioctl_create_enclave *) arg;
  long long unsigned untrusted_size = enclp->utm_size;

  enclave = get_enclave_by_id(enclp->eid);

  if(!enclave) {
    keystone_err("invalid enclave id\n");
    return -EINVAL;
  }

  utm = kmalloc(sizeof(struct utm), GFP_KERNEL);
  if (!utm) {
    ret = -ENOMEM;
    return ret;
  }

  ret = utm_init(utm, untrusted_size);

  /* prepare for mmap */
  enclave->utm = utm;

  enclp->utm_paddr = __pa(utm->ptr);

  return ret;
}

static int __keystone_destroy_enclave(unsigned int ueid)
{
  struct sbiret ret;
  struct enclave *enclave;
  enclave = get_enclave_by_id(ueid);

  if (!enclave) {
    keystone_err("invalid enclave id\n");
    return -EINVAL;
  }

  if (enclave->eid != KEYSTONE_INVALID_EID) {
    ret = sbi_sm_destroy_enclave(enclave->eid);
    if (ret.error) {
      keystone_err("fatal: cannot destroy enclave: SBI failed with error code %ld\n", ret.error);
      return -EINVAL;
    }
  } else {
    keystone_warn("keystone_destroy_enclave: skipping (enclave does not exist)\n");
  }


  destroy_enclave(enclave);
  enclave_idr_remove(ueid);

  return 0;
}

static int keystone_destroy_enclave(struct file *filep, unsigned long arg)
{
  int ret;
  struct keystone_ioctl_create_enclave *enclp = (struct keystone_ioctl_create_enclave *) arg;
  unsigned long ueid = enclp->eid;

  ret = __keystone_destroy_enclave(ueid);
  if (!ret) {
    filep->private_data = NULL;
  }
  return ret;
}

static int keystone_resume_enclave(unsigned long data)
{
  struct sbiret ret;
  struct keystone_ioctl_run_enclave *arg = (struct keystone_ioctl_run_enclave*) data;
  unsigned long ueid = arg->eid;
  struct enclave* enclave;
  enclave = get_enclave_by_id(ueid);

  if (!enclave)
  {
    keystone_err("invalid enclave id\n");
    return -EINVAL;
  }

  if (enclave->eid == KEYSTONE_INVALID_EID) {
    keystone_err("real enclave does not exist\n");
    return -EINVAL;
  }

  ret = sbi_sm_resume_enclave(enclave->eid);

  arg->error = ret.error;
  arg->value = ret.value;

  return 0;
}

long keystone_ioctl(struct file *filep, unsigned int cmd, unsigned long arg)
{
  long ret;
  unsigned long not_copied;
  char data[512];

  size_t ioc_size;

  if (!arg)
    return -EINVAL;

  ioc_size = _IOC_SIZE(cmd);
  ioc_size = ioc_size > sizeof(data) ? sizeof(data) : ioc_size;

  if (copy_from_user(data,(void __user *) arg, ioc_size))
    return -EFAULT;

  switch (cmd) {
    case KEYSTONE_IOC_CREATE_ENCLAVE:
      ret = keystone_create_enclave(filep, (unsigned long) data);
      break;
    case KEYSTONE_IOC_FINALIZE_ENCLAVE:
      ret = keystone_finalize_enclave((unsigned long) data);
      keystone_info("ioctl: FINALIZE handler returned ret=%ld\n", ret);
      break;
    case KEYSTONE_IOC_DESTROY_ENCLAVE:
      ret = keystone_destroy_enclave(filep, (unsigned long) data);
      break;
    case KEYSTONE_IOC_RUN_ENCLAVE:
      ret = keystone_run_enclave((unsigned long) data);
      break;
    case KEYSTONE_IOC_RESUME_ENCLAVE:
      ret = keystone_resume_enclave((unsigned long) data);
      break;
    /* Note that following commands could have been implemented as a part of ADD_PAGE ioctl.
     * However, there was a weird bug in compiler that generates a wrong control flow
     * that ends up with an illegal instruction if we combine switch-case and if statements.
     * We didn't identified the exact problem, so we'll have these until we figure out */
    case KEYSTONE_IOC_UTM_INIT:
      ret = utm_init_ioctl(filep, (unsigned long) data);
      break;
    default:
      return -ENOSYS;
  }

  if (cmd == KEYSTONE_IOC_FINALIZE_ENCLAVE) {
    keystone_info("ioctl: FINALIZE before copy pid=%d comm=%s "
                  "arg=0x%lx size=%zu mm=%px active_mm=%px access_ok=%d "
                  "preempt_count=0x%x irqs_disabled=%d pagefault_disabled=%d "
                  "sstatus=0x%lx satp=0x%lx\n",
                  current->pid, current->comm, arg, ioc_size, current->mm,
                  current->active_mm,
                  access_ok((void __user *)arg, ioc_size), preempt_count(),
                  irqs_disabled(), pagefault_disabled(),
                  csr_read(CSR_STATUS), csr_read(CSR_SATP));
    keystone_info("ioctl: FINALIZE copy_to_user begin size=%zu\n", ioc_size);
  }
  
  // Debug: TLB flush
  keystone_info("FINALIZE: global TLB flush begin\n");
  local_flush_tlb_all();
  keystone_info("FINALIZE: global TLB flush end\n");

  not_copied = copy_to_user((void __user*) arg, data, ioc_size);
  if (cmd == KEYSTONE_IOC_FINALIZE_ENCLAVE)
    keystone_info("ioctl: FINALIZE copy_to_user returned not_copied=%lu "
                  "pid=%d sstatus=0x%lx satp=0x%lx\n",
                  not_copied, current->pid, csr_read(CSR_STATUS),
                  csr_read(CSR_SATP));

  if (not_copied) {
    if (cmd == KEYSTONE_IOC_FINALIZE_ENCLAVE)
      keystone_err("ioctl: FINALIZE copy_to_user failed\n");
    return -EFAULT;
  }

  if (cmd == KEYSTONE_IOC_FINALIZE_ENCLAVE)
    keystone_info("ioctl: FINALIZE returning to userspace ret=%ld\n", ret);

  return ret;
}

int keystone_release(struct inode *inode, struct file *file) {
  unsigned long ueid = (unsigned long)(file->private_data);
  struct enclave *enclave;

  /* enclave has been already destroyed */
  if (!ueid) {
    return 0;
  }

  /* We need to send destroy enclave just the eid to close. */
  enclave = get_enclave_by_id(ueid);

  if (!enclave) {
    /* If eid is set to the invalid id, then we do not do anything. */
    return -EINVAL;
  }
  if (enclave->close_on_pexit) {
    return __keystone_destroy_enclave(ueid);
  }
  return 0;
}
