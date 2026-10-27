/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_SOC_MEDIATEK_MT6580_MAINPLL_H
#define __LINUX_SOC_MEDIATEK_MT6580_MAINPLL_H

#include <linux/types.h>

struct device;

struct mt6580_axi_context {
	u32 mux_before, mux_parked, mux_wake, mux_after;
	u32 cpu_mux_before, cpu_mux_after;
	u32 pll_before[5], pll_after[5];
	u32 pll_off[5], pll_wake[5];
	bool prepared, parked;
	bool mainpll_requested, mainpll_off;
	bool mainpll_auto_restored, mainpll_enable_written;
	u32 entries, resumes;
	u32 power_downs, power_ups;
	u32 dcm_before[6], dcm_after[6];
	u32 dcm_pauses, dcm_restores;
	bool dcm_active, dcm_paused;
	int error;
};

/* Caller holds bound TOPCKGEN/APMIXEDSYS suppliers across the transaction.
 * Prepare/enter/actual restore require CPU0 only and local IRQs disabled.
 * Enter runs in the coherency-off finisher; resume precedes L2/device restore.
 * mainpll_requested is caller input, unchanged by prepare. Only that explicit
 * request power-cycles MAINPLL after verified AXI parking. No helper changes
 * PCW/posdiv, AP_PLL controls, divider enables, audio, D14 or memory PLL/timings.
 * Unknown power bits are preserved by RMW, checked only after power-up, and
 * never rewritten to match a saved value; they may change as hardware status.
 * entries counts mux write attempts; resumes counts verified restorations.
 * Power counters count verified down/recovery transactions; power_ups includes
 * hardware-preempted recovery, identified by mainpll_auto_restored. The
 * mainpll_enable_written flag distinguishes a retry after software enable.
 * Owned CPU/bus DCM is paused before AXI/PLL changes and restored only after
 * verified clocks return. dcm_paused retains partial-write ownership on error;
 * retry cleanup must also handle it when AXI was never parked.
 */
int mt6580_axi_sleep_prepare(struct device *top, struct device *pll,
			     struct mt6580_axi_context *ctx);
int mt6580_axi_sleep_enter(struct device *top, struct device *pll,
			   struct mt6580_axi_context *ctx);
int mt6580_axi_sleep_resume(struct device *top, struct device *pll,
			    struct mt6580_axi_context *ctx);

#endif
