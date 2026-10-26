/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6580_CPU_PM_H
#define __LINUX_SOC_MEDIATEK_MT6580_CPU_PM_H

#include <linux/types.h>

struct mt6580_cpu0_context {
	u32 boot_before, boot_resume, boot_after;
	u32 timer_ctrl, timer_ctrl_after;
	u64 timer_cval, counter_before, counter_after;
	u32 resumes;
	int result;
};

int mt6580_cpu0_resume_entry(struct mt6580_cpu0_context *ctx,
			   int (*idle)(unsigned long), unsigned long arg);

#endif
