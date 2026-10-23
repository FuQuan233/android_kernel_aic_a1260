#ifndef MALI_COMPAT_618_H
#define MALI_COMPAT_618_H
#include <linux/version.h>
#include <linux/dma-fence.h>
#include <linux/wait.h>
#define wait_queue_t wait_queue_entry_t
#include <linux/mm.h>
#include <linux/string.h>
#define strlcpy strscpy
static inline int vm_insert_pfn(struct vm_area_struct *vma, unsigned long addr, unsigned long pfn)
{
 vm_fault_t ret = vmf_insert_pfn(vma, addr, pfn);
 if (ret & VM_FAULT_OOM) return -ENOMEM;
 if (ret & VM_FAULT_ERROR) return -EFAULT;
 return 0;
}
#endif
