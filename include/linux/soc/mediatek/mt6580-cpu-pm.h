/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6580_CPU_PM_H
#define __LINUX_SOC_MEDIATEK_MT6580_CPU_PM_H

#include <linux/types.h>

struct mt6580_cpu0_context {
	u32 boot_before, boot_resume, boot_after;
	u32 timer_ctrl, timer_ctrl_after;
	u64 timer_cval, counter_before, counter_after;
	u32 resumes;
	u32 cold_resumes, aborts, errata_runs;
	u32 retained_resumes;
	u32 banked[32], l2ctlr, l2ctlr_after, cache_config, cache_after;
	u32 wdt_before, wdt_armed, wdt_after, wdt_length;
	int result;
};

int mt6580_cpu0_suspend(struct mt6580_cpu0_context *ctx, unsigned int mode,
		       int (*idle)(unsigned long), int (*wait)(unsigned long),
		       unsigned long arg);

/* Bounded runtime CPU0 power-down, with L2 and watchdog state retained. */
int mt6580_cpu0_idle(struct mt6580_cpu0_context *ctx,
		     int (*idle)(unsigned long), int (*wait)(unsigned long),
		     unsigned long arg);

/* Prepare before timekeeping suspend; account before syscore resume. */
int mt6580_suspend_clock_prepare(void);
void mt6580_suspend_clock_add(u64 stopped_ns);

#endif
