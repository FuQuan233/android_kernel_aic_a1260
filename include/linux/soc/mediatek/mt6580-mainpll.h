/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6580_MAINPLL_H
#define __LINUX_SOC_MEDIATEK_MT6580_MAINPLL_H

#include <linux/types.h>

struct device;

struct mt6580_axi_context {
	u32 mux_before, mux_parked, mux_wake, mux_after;
	u32 cpu_mux_before, cpu_mux_after;
	u32 pll_before[5], pll_after[5];
	bool prepared, parked;
	u32 entries, resumes;
	int error;
};

/* Caller holds bound TOPCKGEN/APMIXEDSYS suppliers across the transaction.
 * Prepare/enter/actual restore require CPU0 only and local IRQs disabled.
 * Enter runs in the coherency-off finisher; resume precedes L2/device restore.
 * No helper writes PLL, divider, audio, D14 or memory clock registers.
 * entries counts mux write attempts; resumes counts verified restorations.
 */
int mt6580_axi_sleep_prepare(struct device *top, struct device *pll,
			     struct mt6580_axi_context *ctx);
int mt6580_axi_sleep_enter(struct device *top, struct device *pll,
			   struct mt6580_axi_context *ctx);
int mt6580_axi_sleep_resume(struct device *top, struct device *pll,
			    struct mt6580_axi_context *ctx);

#endif
