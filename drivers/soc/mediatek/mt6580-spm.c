// SPDX-License-Identifier: GPL-2.0-only
/* MT6580 retained-power PCM execution and PWRAP suspend handoff. */
#include <linux/dma-mapping.h>
#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/irq.h>
#include <linux/math64.h>
#include <linux/moduleparam.h>
#include <linux/cpuidle.h>
#include <linux/cpu.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/panic.h>
#include <linux/smp.h>
#include <linux/sched.h>
#include <linux/soc/mediatek/mt6580-idle.h>
#include <linux/soc/mediatek/mt6580-pwrap.h>
#include <linux/soc/mediatek/mt6580-uart.h>
#include <linux/soc/mediatek/mt6580-usb.h>
#include <linux/soc/mediatek/mt6580-cpu-pm.h>
#include <linux/soc/mediatek/mt6580-ddr.h>
#include <linux/soc/mediatek/mt6580-irq.h>
#include <linux/soc/mediatek/mt6580-mainpll.h>
#include <linux/suspend.h>

#include <asm/arch_timer.h>
#include <asm/proc-fns.h>

#include "mt6580-spm-pcm.h"
#include "mt6580-idle-pcm.h"

#define PCM_CON0		0x310
#define PCM_CON1		0x314
#define PCM_IM_PTR		0x318
#define PCM_IM_LEN		0x31c
#define PCM_PWR_IO_EN		0x358
#define PCM_DATA_INI		0x320
#define PCM_MAS_PAUSE_MASK	0x354
#define PCM_TIMER_VAL		0x35c
#define PCM_TIMER_OUT		0x360
#define PCM_REG0		0x380
#define PCM_REG11		0x3ac
#define PCM_REG12		0x3b0
#define PCM_REG13		0x3b4
#define PCM_PC			0x3bc
#define PCM_EVENT_STA		0x3c0
#define PCM_FSM_STA		0x3c4
#define PCM_SW_INT_CLEAR	0x3e4
#define SPM_CLK_CON		0x400
#define SPM_CLK_SETTLE		0x100
#define SPM_SYSCLK_SETTLE	99 /* BSP 32 kHz ticks, approximately 3 ms. */
#define SPM_SYSSETTLE_SEL	BIT(4)
#define SPM_SRCLKENA_MASK	BIT(6)
#define SPM_CXO32K_MD1		BIT(9)
#define SPM_POWER_ON_VAL0	0x010
#define SPM_POWER_ON_VAL1	0x014
#define SPM_AP_STANDBY		0x608
#define SPM_PWR_STATUS		0x60c
#define SPM_PWR_STATUS2		0x610
#define SPM_DPY_PWR_CON		0x240
#define SPM_IFR_PWR_CON		0x234
#define SPM_IFR_SRAMROM_CON	0x294
#define SPM_WAKE_MASK		0x810
#define SPM_CPU_WAKE		0x814
#define SPM_ISR_MASK		0x900
#define SPM_ISR_STATUS		0x904
#define SPM_RAW_STA		0x910
#define SPM_WAKE_MISC		0x918
#define PCM_FLAGS		0xb08
#define PCM_SRC_REQ		0xb0c
#define PCM_DEBUG_FLAG		0xb18
#define PCM_MMDDR_MASK		0xb1c
#define PCM_KICK		BIT(0)
#define PCM_KEY			(0x0b16 << 16)
#define PCM_IM_KICK		BIT(1)
#define PCM_IM_SLEEP_DVS	BIT(3)
#define PCM_SW_RESET		BIT(15)
#define PCM_FSM_DEFAULT		0x48490
#define PCM_FSM_MASK		GENMASK(21, 0)
#define PCM_IM_STATE_MASK	GENMASK(9, 7)
#define PCM_IM_READY		BIT(9)
#define PCM_TIMER_EN		BIT(5)
#define PCM_R0_OUTPUT		BIT(0)
#define PCM_R7_OUTPUT		BIT(7)
#define PCM_RETURN		BIT(3)
#define PCM_SW_INT0		BIT(4)
#define PCM_IRQ_MASK		(GENMASK(15, 8) | PCM_RETURN)
#define PCM_APSRC_REQ		BIT(0)
#define PCM_F26M_REQ		BIT(1)
#define F26M_KEEP_UART		BIT(0)
#define F26M_KEEP_DOMAIN	BIT(1)
#define F26M_KEEP_XTAL		BIT(2)
#define F26M_KEEP_FIRMWARE	BIT(3)
#define F26M_KEEP_USB		BIT(4)
/* BSP CPU/infra/DDRPHY/VCORE/PASR/DPD/SODI-EMI/MAINPLL/CPU-DVS disable. */
#define PCM_RETAIN_FLAGS	(GENMASK(5, 0) | GENMASK(9, 7))
#define PCM_WAKE_SOURCES	(BIT(0) | BIT(2) | BIT(3) | BIT(5) | BIT(14) | \
				 BIT(15) | BIT(22) | BIT(26))

static const u16 pcm_saved_regs[] = {
	PCM_CON0, PCM_CON1, PCM_IM_PTR, PCM_IM_LEN,
	0x340, 0x344, 0x348, 0x34c, 0x3d0, 0x3d4, 0x3d8, 0x3dc,
	PCM_DATA_INI, PCM_MAS_PAUSE_MASK, PCM_TIMER_VAL, SPM_CLK_CON, SPM_CLK_SETTLE,
	SPM_AP_STANDBY, SPM_WAKE_MASK, SPM_CPU_WAKE, SPM_ISR_MASK,
	PCM_FLAGS, PCM_SRC_REQ, 0xb00, 0xb04, PCM_DEBUG_FLAG,
	0xb60, 0xb64, 0xb68, 0xb6c,
	0xf00, 0xf04, 0xf08, 0xf0c, 0xf10, 0xf14, 0xf18, 0xf1c,
};

struct mt6580_pcm_wake {
	u32 cpu0_mode;
	u32 r11, r12, r13, raw, misc, timer, isr, event, pc, assert_pc, debug;
	u32 power_before, power_after, power2_before, power2_after, dvfs;
	u32 timer_con_start, timer_val_start, timer_con_wake, timer_val_wake;
	u32 timer_at_wake, sleep_sta, fsm_at_wake;
	u32 pwr_io, flags, src_req, flags_expected;
	u32 pwr_io_expected, r0, r0_blockers;
	u32 src_before, src_expected, src_blockers, r13_before, usb_ready;
	u32 poweron0, poweron1, clk_con, standby, mmddr;
	u32 settle_before, settle_md, settle_programmed, settle_applied;
	u32 settle_wake, settle_restored;
	u32 ddr_before[4], ddr_after[4];
	u32 clk_expected, clk_wake, clk_restored;
	u32 dpy_before, dpy_after;
	u32 ifr_before, ifr_after, ifrsram_before, ifrsram_after, infra_blockers;
	u32 time_ticks_start, time_ticks_end;
	u64 time_counter_start, time_counter_end, stopped_ns;
	bool time_started, pcm_started;
	int ddr_check_before, ddr_check_after;
	bool irq_pending, forced_exit;
	bool r0_requested, settle_changed;
	bool apsrc_requested;
	bool f26m_requested;
	bool ddrphy_requested;
	bool infra_requested;
	bool axi26_requested;
	bool mainpll_requested;
};

struct mt6580_runtime_idle {
	void *pcm;
	dma_addr_t dma;
	u32 saved[ARRAY_SIZE(pcm_saved_regs)];
	struct mt6580_cpu0_context cpu;
	struct mt6580_gic_idle_context gic;
	u32 attempts, entries, pcm_runs, rejected, failures;
	u32 blocked_cpus, blocked_domains, blocked_irq, blocked_pcm, blocked_cirq, blocked_gic;
	int admission_error;
	u32 r11, r12, assert_pc, ticks, isr, forced;
	u32 power_before, power_after, power2_before, power2_after;
	u64 counter_start, counter_end, max_ticks;
	int error;
	bool started, disabled, powerdown;
};

/* Qualification can isolate the PCM/interrupt transaction from CPU reset. */
static bool runtime_cpu_powerdown = true;
module_param(runtime_cpu_powerdown, bool, 0644);
MODULE_PARM_DESC(runtime_cpu_powerdown, "Allow CPU0 power-down in runtime idle");

struct mt6580_spm {
	struct device *dev;
	struct device *pwrap;
	struct device *ddr;
	/* TOPCKGEN followed by APMIXED, both owned by the clock provider. */
	struct device *clock_ctl[2];
	struct device *uarts[2];
	unsigned int num_uarts;
	struct device *usbs[1];
	unsigned int num_usbs;
	void __iomem *base;
	void *pcm;
	dma_addr_t pcm_dma;
	u32 saved[ARRAY_SIZE(pcm_saved_regs)];
	u32 loaded_fsm, restored_fsm;
	unsigned int loads;
	unsigned int runs, irqs, timer_ms;
	unsigned int cpu0_mode;
	bool r0_control;
	bool apsrc_control;
	bool f26m_control;
	bool ddrphy_control;
	bool infra_control;
	bool axi_26m_control;
	bool mainpll_control;
	unsigned int voltage_bank_control;
	bool vproc_retention;
	suspend_state_t requested_state;
	struct mt6580_axi_context axi;
	bool sleep_clock_ready;
	bool suspend_active;
	suspend_state_t last_state;
	unsigned int deep_requests, deep_fallbacks;
	u32 fallback_source, fallback_r0, fallback_infra;
	bool deep_fallback;
	struct mt6580_cpu0_context cpu0;
	u32 irq_status;
	int irq;
	bool irq_wake;
	struct mt6580_pcm_wake wake;
	struct mt6580_runtime_idle idle;
	int last_error;
};

/* There is one system suspend provider, bound for the kernel's lifetime. */
static struct mt6580_spm *mt6580_spm;

static int mt6580_pcm_idle(struct mt6580_spm *spm)
{
	if (readl(spm->base + PCM_PWR_IO_EN) || readl(spm->base + SPM_CPU_WAKE) ||
	    readl(spm->base + 0x760) & BIT(0) ||
	    readl(spm->base + SPM_ISR_STATUS) & (PCM_RETURN | PCM_SW_INT0) ||
	    (readl(spm->base + PCM_FSM_STA) & PCM_FSM_MASK) != PCM_FSM_DEFAULT)
		return -EBUSY;
	return 0;
}

static int mt6580_pcm_reset(struct mt6580_spm *spm)
{
	u32 val;

	writel(PCM_KEY | PCM_SW_RESET, spm->base + PCM_CON0);
	writel(PCM_KEY, spm->base + PCM_CON0);
	return readl_poll_timeout_atomic(spm->base + PCM_FSM_STA, val,
			(val & PCM_FSM_MASK) == PCM_FSM_DEFAULT, 1, 10000);
}

static const u16 pcm_vector_regs[] = {
	0x340, 0x344, 0x348, 0x34c, 0x3d0, 0x3d4, 0x3d8, 0x3dc,
};

static int mt6580_pcm_load_image(struct mt6580_spm *spm, dma_addr_t dma,
				unsigned int words, const u32 *vectors,
				unsigned int nr_vectors)
{
	u32 val;
	int i, ret;

	ret = mt6580_pcm_reset(spm);
	if (ret)
		return ret;
	writel(PCM_KEY | PCM_IM_SLEEP_DVS, spm->base + PCM_CON0);
	/* BSP IM fetch: event lock, SRAM retention, non-replace and APB access.
	 * PCM timer/watchdog are off; no PCM_KICK or power-output enable.
	 */
	writel(PCM_KEY | BIT(3) | BIT(6) | BIT(10) | BIT(11) | BIT(12) | BIT(14),
	       spm->base + PCM_CON1);
	writel(lower_32_bits(dma), spm->base + PCM_IM_PTR);
	writel(words - 1, spm->base + PCM_IM_LEN);
	/* Coherent DMA buffer must be visible before starting instruction fetch. */
	dma_wmb();
	writel(PCM_KEY | PCM_IM_SLEEP_DVS | PCM_IM_KICK, spm->base + PCM_CON0);
	writel(PCM_KEY | PCM_IM_SLEEP_DVS, spm->base + PCM_CON0);
	ret = readl_poll_timeout_atomic(spm->base + PCM_FSM_STA, val,
			(val & PCM_IM_STATE_MASK) == PCM_IM_READY, 1, 10000);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(pcm_vector_regs); i++)
		writel(i < nr_vectors ? vectors[i] : 0, spm->base + pcm_vector_regs[i]);
	if (readl(spm->base + PCM_PWR_IO_EN))
		return -EIO;
	return 0;
}

static int mt6580_pcm_load(struct mt6580_spm *spm)
{
	int ret = mt6580_pcm_load_image(spm, spm->pcm_dma,
			ARRAY_SIZE(mt6580_suspend_pcm), mt6580_pcm_vectors,
			ARRAY_SIZE(mt6580_pcm_vectors));

	spm->loaded_fsm = readl(spm->base + PCM_FSM_STA);
	if (!ret)
		spm->loads++;
	return ret;
}

static int mt6580_pcm_restore_regs(struct mt6580_spm *spm, const u32 *saved,
				  int *irq_error)
{
	int i, ret;
	u32 value;

	/* Stop our interrupt source before resetting/restoring its controller. */
	writel(0, spm->base + PCM_PWR_IO_EN);
	writel(readl(spm->base + SPM_ISR_MASK) | PCM_IRQ_MASK, spm->base + SPM_ISR_MASK);
	writel(PCM_KEY | (readl(spm->base + PCM_CON1) & ~PCM_TIMER_EN),
	       spm->base + PCM_CON1);
	writel(~0U, spm->base + SPM_WAKE_MASK);
	writel(0, spm->base + SPM_CPU_WAKE);
	writel(PCM_RETURN, spm->base + SPM_ISR_STATUS);
	writel(1, spm->base + PCM_SW_INT_CLEAR);
	*irq_error = irq_set_irqchip_state(spm->irq, IRQCHIP_STATE_PENDING, false);
	ret = mt6580_pcm_reset(spm);
	if (ret)
		return ret;
	/* Restore data/vector registers first and controls last. Never replay a
	 * fetch/run/reset command from a sampled control register.
	 */
	for (i = 2; i < ARRAY_SIZE(pcm_saved_regs); i++) {
		if (pcm_saved_regs[i] == SPM_CLK_SETTLE)
			continue;
		writel(saved[i], spm->base + pcm_saved_regs[i]);
	}
	value = saved[1] & 0xffff;
	writel(PCM_KEY | value, spm->base + PCM_CON1);
	value = saved[0] & 0xffff & ~(BIT(0) | PCM_IM_KICK | PCM_SW_RESET);
	writel(PCM_KEY | value, spm->base + PCM_CON0);
	ret = mt6580_pcm_idle(spm);
	return ret;
}

static int mt6580_pcm_restore(struct mt6580_spm *spm)
{
	int irq_error;
	int ret = mt6580_pcm_restore_regs(spm, spm->saved, &irq_error);

	if (ret)
		return ret;
	spm->restored_fsm = readl(spm->base + PCM_FSM_STA);
	spm->wake.clk_restored = readl(spm->base + SPM_CLK_CON);
	if (spm->wake.settle_changed) {
		/* Readback includes the MD contribution, not just the SW field.
		 * Use the BSP's unsigned difference for both setup and rollback.
		 */
		writel(spm->wake.settle_before - spm->wake.settle_md,
		       spm->base + SPM_CLK_SETTLE);
		ret = readl_poll_timeout_atomic(spm->base + SPM_CLK_SETTLE,
			spm->wake.settle_restored,
			spm->wake.settle_restored == spm->wake.settle_before,
			1, 1000);
		if (ret)
			return ret;
		if (spm->wake.clk_restored != spm->wake.clk_con)
			return -EIO;
	} else {
		spm->wake.settle_restored = readl(spm->base + SPM_CLK_SETTLE);
	}
	ret = mt6580_pcm_idle(spm);
	return ret ? ret : irq_error;
}

/* Runtime idle never calls device PM, regmap, CCF, or timekeeping suspend.
 * A running watchdog stays owned by its driver. Its independent PCM timer
 * bounds residency to 20 ms; the normal broadcast event can wake us earlier.
 */
static int __cpuidle mt6580_idle_wait(unsigned long arg)
{
	struct mt6580_spm *spm = (void *)arg;
	u32 val;

	if (!(readl(spm->base + SPM_ISR_STATUS) & PCM_RETURN)) {
		spm->idle.forced++;
		writel(1, spm->base + SPM_CPU_WAKE);
	}
	return readl_poll_timeout_atomic(spm->base + SPM_ISR_STATUS, val,
					val & PCM_RETURN, 1, 10000);
}

static int __cpuidle mt6580_idle_finish(unsigned long arg)
{
	struct mt6580_spm *spm = (void *)arg;
	u32 con0;

	/* Catch an interrupt or a hotplug attempt before giving PCM ownership.
	 * A later edge remains latched in CIRQ; a pending level aborts WFI.
	 */
	if (need_resched() || num_online_cpus() != 1 || mt6580_gic_idle_ready())
		return -EBUSY;
	con0 = readl(spm->base + PCM_CON0) & ~(PCM_KICK | PCM_IM_KICK);
	spm->idle.started = true;
	writel(PCM_KEY | con0 | PCM_KICK, spm->base + PCM_CON0);
	writel(PCM_KEY | con0, spm->base + PCM_CON0);
	cpu_do_idle();
	return 0;
}

int mt6580_spm_cpu0_idle(void)
{
	struct mt6580_spm *spm = READ_ONCE(mt6580_spm);
	struct mt6580_runtime_idle *idle;
	u32 con1, src, power, power2;
	int i, ret, restore, gic_restore, irq_restore;

	if (!spm || !irqs_disabled())
		return -ENODEV;
	idle = &spm->idle;
	if (system_state != SYSTEM_RUNNING)
		return -EBUSY;
	if (READ_ONCE(idle->disabled))
		return -EIO;
	if (READ_ONCE(spm->suspend_active))
		return -EBUSY;
	/* Match the MT6580 BSP single-CPU criterion, including hardware status.
	 * Never try to turn off an online sibling, or enter during CPU teardown.
	 */
	if (smp_processor_id() || num_online_cpus() != 1) {
		idle->blocked_cpus++;
		return -EBUSY;
	}
	power = readl(spm->base + SPM_PWR_STATUS);
	power2 = readl(spm->base + SPM_PWR_STATUS2);
	if ((power | power2) & GENMASK(12, 10)) {
		idle->blocked_cpus++;
		return -EBUSY;
	}
	/* Initial runtime profile admits no active display/GPU transaction. */
	if ((power | power2) & (BIT(3) | BIT(4))) {
		idle->blocked_domains++;
		return -EBUSY;
	}
	if (need_resched() || mt6580_gic_idle_ready()) {
		idle->blocked_irq++;
		return -EBUSY;
	}
	if (mt6580_pcm_idle(spm)) {
		idle->blocked_pcm++;
		return -EBUSY;
	}
	ret = mt6580_cirq_idle_enter();
	if (ret) {
		idle->blocked_cirq++;
		idle->admission_error = ret;
		return ret;
	}
	ret = mt6580_gic_idle_enter(&idle->gic, spm->irq);
	if (ret) {
		idle->blocked_gic++;
		idle->admission_error = ret;
		mt6580_cirq_idle_exit();
		return ret;
	}
	idle->attempts++;
	idle->admission_error = 0;
	idle->powerdown = READ_ONCE(runtime_cpu_powerdown);
	idle->started = false;
	idle->power_before = power;
	idle->power2_before = power2;
	idle->counter_start = __arch_counter_get_cntpct();
	for (i = 0; i < ARRAY_SIZE(pcm_saved_regs); i++)
		idle->saved[i] = readl(spm->base + pcm_saved_regs[i]);
	ret = mt6580_pcm_load_image(spm, idle->dma,
			ARRAY_SIZE(mt6580_idle_pcm), mt6580_idle_vectors,
			ARRAY_SIZE(mt6580_idle_vectors));
	if (ret)
		goto restore;
	/* Retain R0/crystal control, APSRC/F26M, infra, DDRPHY and all PLLs.
	 * Only CPU0 power control is connected to PCM's R7 output.
	 */
	writel(readl(spm->base + SPM_POWER_ON_VAL0), spm->base + PCM_DATA_INI);
	writel(BIT(16), spm->base + PCM_PWR_IO_EN);
	writel(0, spm->base + PCM_PWR_IO_EN);
	writel(readl(spm->base + SPM_POWER_ON_VAL1), spm->base + PCM_DATA_INI);
	writel(BIT(23), spm->base + PCM_PWR_IO_EN);
	writel(0, spm->base + PCM_PWR_IO_EN);
	writel(0, spm->base + PCM_DATA_INI);
	writel(~0U, spm->base + PCM_MAS_PAUSE_MASK);
	writel(PCM_RETAIN_FLAGS & ~(idle->powerdown ? BIT(0) : 0), spm->base + PCM_FLAGS);
	writel(0, spm->base + 0xb00);
	src = readl(spm->base + PCM_SRC_REQ) | PCM_APSRC_REQ | PCM_F26M_REQ;
	writel(src, spm->base + PCM_SRC_REQ);
	writel(readl(spm->base + SPM_CLK_CON) | BIT(5), spm->base + SPM_CLK_CON);
	writel((readl(spm->base + SPM_AP_STANDBY) & ~GENMASK(7, 4)) | BIT(4) | BIT(6),
	       spm->base + SPM_AP_STANDBY);
	for (i = 0; i < 8; i++)
		writel(!i, spm->base + 0xf00 + i * 4);
	/* GPT is the generic broadcast event. CIRQ preserves external edges.
	 * CPU0_IRQ covers a level becoming pending at the entry boundary.
	 */
	writel(~(BIT(0) | BIT(4) | BIT(22) | BIT(26)), spm->base + SPM_WAKE_MASK);
	writel(PCM_RETURN, spm->base + SPM_ISR_STATUS);
	writel(1, spm->base + PCM_SW_INT_CLEAR);
	writel((readl(spm->base + SPM_ISR_MASK) & BIT(2)) | GENMASK(15, 9),
	       spm->base + SPM_ISR_MASK);
	writel(655, spm->base + PCM_TIMER_VAL);
	con1 = readl(spm->base + PCM_CON1) | PCM_TIMER_EN;
	writel(PCM_KEY | con1, spm->base + PCM_CON1);
	writel(PCM_R7_OUTPUT, spm->base + PCM_PWR_IO_EN);
	if (idle->powerdown)
		ret = mt6580_cpu0_idle(&idle->cpu, mt6580_idle_finish, mt6580_idle_wait,
				     (unsigned long)spm);
	else {
		if (is_idle_task(current))
			ct_cpuidle_enter();
		ret = mt6580_idle_finish((unsigned long)spm);
		if (is_idle_task(current))
			ct_cpuidle_exit();
	}
	if (idle->started) {
		/* Even a cache/notifier abort must release PCM power ownership. */
		restore = mt6580_idle_wait((unsigned long)spm);
		if (restore)
			panic("MT6580 runtime PCM failed to release CPU power: %d", restore);
		idle->r11 = readl(spm->base + PCM_REG11);
		idle->r12 = readl(spm->base + PCM_REG12);
		idle->assert_pc = readl(spm->base + PCM_DATA_INI);
		idle->ticks = readl(spm->base + PCM_TIMER_OUT);
		idle->isr = readl(spm->base + SPM_ISR_STATUS);
		idle->power_after = readl(spm->base + SPM_PWR_STATUS);
		idle->power2_after = readl(spm->base + SPM_PWR_STATUS2);
		if (!ret && (idle->assert_pc || idle->r11 != 0x55aa55aa ||
			     power != idle->power_after || power2 != idle->power2_after ||
			     readl(spm->base + PCM_FLAGS) !=
				(PCM_RETAIN_FLAGS & ~(idle->powerdown ? BIT(0) : 0)) ||
			     readl(spm->base + PCM_SRC_REQ) != src ||
			     readl(spm->base + PCM_PWR_IO_EN) != PCM_R7_OUTPUT))
			ret = -EIO;
	}
restore:
	restore = mt6580_pcm_restore_regs(spm, idle->saved, &irq_restore);
	if (!restore)
		restore = irq_restore;
	/* cpu_cluster_pm_exit has restored the GIC before flushing edge latches. */
	gic_restore = mt6580_gic_idle_exit(&idle->gic);
	mt6580_cirq_idle_exit();
	idle->counter_end = __arch_counter_get_cntpct();
	idle->max_ticks = max(idle->max_ticks, idle->counter_end - idle->counter_start);
	if (restore)
		panic("MT6580 runtime PCM rollback failed: %d", restore);
	if (gic_restore)
		panic("MT6580 runtime GIC rollback failed: %d", gic_restore);
	/* 26 MHz stays requested: CNT must account for the PCM residency too.
	 * Allow one millisecond for the independent 32 kHz sample boundaries.
	 */
	if (!ret && idle->started &&
	    mul_u64_u32_div(idle->counter_end - idle->counter_start,
			    NSEC_PER_SEC, arch_timer_get_rate()) + NSEC_PER_MSEC <
	    div_u64((u64)idle->ticks * NSEC_PER_SEC, 32768))
		ret = -EIO;
	idle->error = ret == -EBUSY ? 0 : ret;
	if (ret && ret != -EBUSY) {
		idle->failures++;
		WRITE_ONCE(idle->disabled, true);
		return ret;
	}
	if (ret || !idle->started || (idle->powerdown && idle->cpu.result)) {
		idle->rejected++;
		return -EBUSY;
	}
	idle->pcm_runs++;
	if (!idle->powerdown)
		return 1; /* The caller accounts this diagnostic WFI as state 0. */
	idle->entries++;
	return 0;
}

static ssize_t idle_state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mt6580_spm *spm = dev_get_drvdata(dev);
	struct mt6580_runtime_idle *s = &spm->idle;
	ssize_t n;

	n = sysfs_emit(buf, "profile=cpu0-l2-retained version=pcm_deepidle_v14.0_new disabled=%u "
		      "powerdown=%u attempts=%u entries=%u pcm_runs=%u rejected=%u failures=%u error=%d\n",
		      s->disabled, READ_ONCE(runtime_cpu_powerdown), s->attempts,
		      s->entries, s->pcm_runs, s->rejected, s->failures, s->error);
	n += sysfs_emit_at(buf, n, "blocked cpus=%u domains=%u irq=%u pcm=%u cirq=%u gic=%u error=%d\n",
			  s->blocked_cpus, s->blocked_domains, s->blocked_irq, s->blocked_pcm,
			  s->blocked_cirq, s->blocked_gic, s->admission_error);
#if IS_ENABLED(CONFIG_ARM_MT6580_CPUIDLE)
	{
		struct cpuidle_device *cpu0 = per_cpu(cpuidle_devices, 0);
		struct cpuidle_driver *drv = cpu0 ? cpuidle_get_cpu_driver(cpu0) : NULL;

		if (drv && drv->state_count > 2)
			n += sysfs_emit_at(buf, n, "admission online=%u disable=%llu flags=%x latency_ns=%lld\n",
					  num_online_cpus(), cpu0->states_usage[2].disable,
					  drv->states[2].flags, cpuidle_governor_latency_req(0));
	}
#endif
	n += sysfs_emit_at(buf, n, "cpu retained=%u aborted=%u l2_lost=%u errata=%u "
			  "boot=%08x/%08x cache=%08x/%08x timer=%x/%x watchdog=%x/%x\n",
			  s->cpu.retained_resumes, s->cpu.aborts, s->cpu.cold_resumes,
			  s->cpu.errata_runs, s->cpu.boot_before, s->cpu.boot_after,
			  s->cpu.cache_config, s->cpu.cache_after, s->cpu.timer_ctrl,
			  s->cpu.timer_ctrl_after, s->cpu.wdt_before, s->cpu.wdt_after);
	n += sysfs_emit_at(buf, n, "wake r11=%08x r12=%08x assert=%x ticks=%u isr=%x forced=%u "
			  "power=%x/%x power2=%x/%x counter=%llu/%llu max_ticks=%llu\n",
			  s->r11, s->r12, s->assert_pc, s->ticks, s->isr, s->forced,
			  s->power_before, s->power_after, s->power2_before, s->power2_after,
			  s->counter_start, s->counter_end, s->max_ticks);
	return n;
}
static DEVICE_ATTR_RO(idle_state);

static int mt6580_pcm_capture(struct mt6580_spm *spm)
{
	struct mt6580_pcm_wake *wake = &spm->wake;

	wake->r0 = readl(spm->base + PCM_REG0);
	wake->settle_wake = readl(spm->base + SPM_CLK_SETTLE);
	wake->clk_wake = readl(spm->base + SPM_CLK_CON);
	wake->dpy_after = readl(spm->base + SPM_DPY_PWR_CON);
	wake->ifr_after = readl(spm->base + SPM_IFR_PWR_CON);
	wake->ifrsram_after = readl(spm->base + SPM_IFR_SRAMROM_CON);
	wake->r11 = readl(spm->base + PCM_REG11);
	wake->r12 = readl(spm->base + PCM_REG12);
	wake->r13 = readl(spm->base + PCM_REG13);
	wake->raw = readl(spm->base + SPM_RAW_STA);
	wake->misc = readl(spm->base + SPM_WAKE_MISC);
	wake->timer = readl(spm->base + PCM_TIMER_OUT);
	wake->isr = readl(spm->base + SPM_ISR_STATUS);
	wake->event = readl(spm->base + PCM_EVENT_STA);
	wake->pc = readl(spm->base + PCM_PC);
	wake->assert_pc = readl(spm->base + PCM_DATA_INI);
	wake->debug = readl(spm->base + PCM_DEBUG_FLAG);
	wake->dvfs = readl(spm->base + 0x604);
	wake->power_after = readl(spm->base + SPM_PWR_STATUS);
	wake->power2_after = readl(spm->base + SPM_PWR_STATUS2);
	wake->pwr_io = readl(spm->base + PCM_PWR_IO_EN);
	wake->flags = readl(spm->base + PCM_FLAGS);
	wake->src_req = readl(spm->base + PCM_SRC_REQ);
	return irq_get_irqchip_state(spm->irq, IRQCHIP_STATE_PENDING, &wake->irq_pending);
}

static void mt6580_pcm_source_requests(struct mt6580_spm *spm)
{
	struct mt6580_pcm_wake *wake = &spm->wake;
	unsigned int i;

	wake->src_before = readl(spm->base + PCM_SRC_REQ);
	wake->r13_before = readl(spm->base + PCM_REG13);
	wake->poweron0 = readl(spm->base + SPM_POWER_ON_VAL0);
	wake->poweron1 = readl(spm->base + SPM_POWER_ON_VAL1);
	wake->clk_con = readl(spm->base + SPM_CLK_CON);
	wake->standby = readl(spm->base + SPM_AP_STANDBY);
	wake->mmddr = readl(spm->base + PCM_MMDDR_MASK);
	for (i = 0; i < spm->num_uarts; i++)
		if (!mt6580_uart_sleep_ready(spm->uarts[i]))
			wake->src_blockers |= F26M_KEEP_UART;
	for (i = 0; i < spm->num_usbs; i++) {
		if (mt6580_usb_sleep_ready(spm->usbs[i]))
			wake->usb_ready |= BIT(i);
		else
			wake->src_blockers |= F26M_KEEP_USB;
	}
	if ((wake->power_before | wake->power2_before) & (BIT(3) | BIT(4)))
		wake->src_blockers |= F26M_KEEP_DOMAIN;
	/* Only remove our software override while crystal control and the
	 * firmware's R7 baseline remain in their qualified retained state.
	 */
	if (wake->poweron0 || wake->poweron1 != 0x60015830)
		wake->src_blockers |= F26M_KEEP_XTAL;
	if (wake->src_before & PCM_F26M_REQ)
		wake->src_blockers |= F26M_KEEP_FIRMWARE;
	/* Preserve every CCIF/event mask and retain APSRC for CPU0/DDR. */
	wake->src_expected = wake->src_before | PCM_APSRC_REQ;
	if (wake->src_blockers)
		wake->src_expected |= PCM_F26M_REQ;
	writel(wake->src_expected, spm->base + PCM_SRC_REQ);
}

static void mt6580_pcm_wake_snapshot(struct mt6580_spm *spm)
{
	spm->wake.timer_con_wake = readl(spm->base + PCM_CON1);
	spm->wake.timer_val_wake = readl(spm->base + PCM_TIMER_VAL);
	spm->wake.timer_at_wake = readl(spm->base + PCM_TIMER_OUT);
	spm->wake.sleep_sta = readl(spm->base + 0x720);
	spm->wake.fsm_at_wake = readl(spm->base + PCM_FSM_STA);
}

static int mt6580_pcm_wait_finish(unsigned long arg)
{
	struct mt6580_spm *spm = (void *)arg;
	u32 val;
	int ret;

	/* PCM must have finished its CPU/TOP sequencing before the BSP's
	 * sibling-core L2 recovery is allowed to access those power registers.
	 */
	mt6580_pcm_wake_snapshot(spm);
	if (!(readl(spm->base + SPM_ISR_STATUS) & PCM_RETURN)) {
		spm->wake.forced_exit = true;
		writel(1, spm->base + SPM_CPU_WAKE);
	}
	ret = readl_poll_timeout_atomic(spm->base + SPM_ISR_STATUS, val,
				       val & PCM_RETURN, 1, 100000);
	/* Run on the parked AXI source until PCM has restored the domains.
	 * Restore before sibling-core/L2 recovery; report a mux error after
	 * that required recovery, without confusing it with a PCM timeout.
	 */
	if (!ret && spm->wake.axi26_requested)
		mt6580_axi_sleep_resume(spm->clock_ctl[0], spm->clock_ctl[1], &spm->axi);
	return ret;
}

static int mt6580_pcm_idle_finish(unsigned long arg)
{
	struct mt6580_spm *spm = (void *)arg;
	u32 con0 = readl(spm->base + PCM_CON0) & ~(PCM_KICK | PCM_IM_KICK);
	int ret;

	if (spm->wake.axi26_requested) {
		ret = mt6580_axi_sleep_enter(spm->clock_ctl[0], spm->clock_ctl[1],
					    &spm->axi);
		if (ret)
			return ret;
	}
	writel(PCM_KEY | con0 | PCM_KICK, spm->base + PCM_CON0);
	writel(PCM_KEY | con0, spm->base + PCM_CON0);
	spm->wake.pcm_started = true;
	if (spm->sleep_clock_ready) {
		spm->wake.time_counter_start = __arch_counter_get_cntpct();
		spm->wake.time_ticks_start = readl(spm->base + PCM_TIMER_OUT);
		spm->wake.time_started = true;
	}
	spm->runs++;
	cpu_do_idle();
	mt6580_pcm_wake_snapshot(spm);
	return 0;
}

static void mt6580_pcm_account_time(struct mt6580_spm *spm)
{
	struct mt6580_pcm_wake *wake = &spm->wake;
	u64 pcm_ns, active_ns;
	u32 ticks;

	if (!wake->time_started)
		return;
	wake->time_ticks_end = readl(spm->base + PCM_TIMER_OUT);
	wake->time_counter_end = __arch_counter_get_cntpct();
	if (!wake->f26m_requested)
		return; /* Physical counter already includes the entire interval. */
	ticks = wake->time_ticks_end - wake->time_ticks_start;
	pcm_ns = div_u64((u64)ticks * NSEC_PER_SEC, 32768);
	active_ns = mul_u64_u32_div(wake->time_counter_end - wake->time_counter_start,
				   NSEC_PER_SEC, arch_timer_get_rate());
	/* Exclude the active entry/exit tails already counted by CNT. Clamp
	 * sub-tick cancellation to zero on an immediate or aborted entry.
	 */
	if (pcm_ns > active_ns)
		wake->stopped_ns = pcm_ns - active_ns;
	mt6580_suspend_clock_add(wake->stopped_ns);
}

static int mt6580_pcm_sysclk_settle(struct mt6580_spm *spm)
{
	struct mt6580_pcm_wake *wake = &spm->wake;

	/* BSP MD-off sequence: readback adds the MD contribution to the SW
	 * field. Unsigned subtraction intentionally compensates that addition;
	 * writing the observed total directly would add the MD delay twice.
	 * Complete this setup before any optional F26M request handoff.
	 */
	writel(readl(spm->base + SPM_CLK_CON) | SPM_SYSSETTLE_SEL,
	       spm->base + SPM_CLK_CON);
	writel(0, spm->base + SPM_CLK_SETTLE);
	wake->settle_changed = true;
	wake->settle_md = readl(spm->base + SPM_CLK_SETTLE);
	wake->settle_programmed = SPM_SYSCLK_SETTLE - wake->settle_md;
	wake->settle_applied = SPM_SYSCLK_SETTLE;
	writel(wake->settle_programmed, spm->base + SPM_CLK_SETTLE);
	return readl_poll_timeout_atomic(spm->base + SPM_CLK_SETTLE,
		wake->settle_wake, wake->settle_wake == wake->settle_applied,
		1, 1000);
}

enum mt6580_pcm_profile {
	MT6580_PCM_QUALIFY,
	MT6580_PCM_DEEP,
	MT6580_PCM_RETAIN,
};

static int mt6580_pcm_run(struct mt6580_spm *spm, enum mt6580_pcm_profile profile)
{
	bool qualify = profile == MT6580_PCM_QUALIFY;
	bool deep = profile == MT6580_PCM_DEEP;
	u32 con1, ticks = qualify ? READ_ONCE(spm->timer_ms) * 32768 / 1000 : 0;
	unsigned int mode = qualify ? READ_ONCE(spm->cpu0_mode) : (deep ? 2 : 0);
	int i, ret, capture, restore;

	spm->wake.cpu0_mode = mode;
	spm->wake.r0_requested = qualify ? READ_ONCE(spm->r0_control) : deep;
	spm->wake.apsrc_requested = qualify ? READ_ONCE(spm->apsrc_control) : deep;
	spm->wake.f26m_requested = qualify ? READ_ONCE(spm->f26m_control) : deep;
	spm->wake.ddrphy_requested = qualify ? READ_ONCE(spm->ddrphy_control) : deep;
	spm->wake.infra_requested = qualify ? READ_ONCE(spm->infra_control) : deep;
	spm->wake.axi26_requested = qualify ? READ_ONCE(spm->axi_26m_control) : deep;
	spm->wake.mainpll_requested = qualify ? READ_ONCE(spm->mainpll_control) : deep;
	if (spm->wake.axi26_requested && (mode != 2 || !spm->wake.r0_requested))
		return -EINVAL;
	if (spm->wake.mainpll_requested && !spm->wake.axi26_requested)
		return -EINVAL;
	if (deep && !spm->sleep_clock_ready)
		return -ENODEV;
	/* Legacy DTs can still use the retained profile without PLL suppliers.
	 * Reject before any output or clock change so automatic mem can fall back.
	 */
	if (deep && (!spm->clock_ctl[0] || !spm->clock_ctl[1]))
		return -EBUSY;
	spm->wake.dpy_before = readl(spm->base + SPM_DPY_PWR_CON);
	spm->wake.ifr_before = readl(spm->base + SPM_IFR_PWR_CON);
	spm->wake.ifrsram_before = readl(spm->base + SPM_IFR_SRAMROM_CON);
	spm->wake.pwr_io_expected = PCM_R7_OUTPUT;
	if (spm->wake.apsrc_requested && !spm->wake.r0_requested)
		return -EINVAL;
	if (spm->wake.f26m_requested && !spm->wake.apsrc_requested)
		return -EINVAL;
	if (spm->wake.ddrphy_requested && !spm->wake.apsrc_requested)
		return -EINVAL;
	/* BSP DPY power-on baseline: both power switches/RST_B on, no isolation
	 * or clock-disable. Do not enter PHY power-down from another state.
	 */
	if (spm->wake.ddrphy_requested && spm->wake.dpy_before != 0x0d)
		return -EBUSY;
	if (spm->wake.infra_requested) {
		if (!spm->wake.ddrphy_requested || !spm->wake.f26m_requested)
			spm->wake.infra_blockers |= BIT(0);
		if ((spm->wake.ifr_before & GENMASK(4, 0)) != 0x0d)
			spm->wake.infra_blockers |= BIT(1);
		for (i = 0; i < spm->num_uarts; i++)
			if (!mt6580_uart_infra_ready(spm->uarts[i]))
				spm->wake.infra_blockers |= BIT(2);
		if (!mt6580_sysirq_suspend_ready())
			spm->wake.infra_blockers |= BIT(3);
		if (!mt6580_cirq_suspend_ready())
			spm->wake.infra_blockers |= BIT(4);
		if (!mt6580_pwrap_infra_ready(spm->pwrap))
			spm->wake.infra_blockers |= BIT(5);
		if (spm->wake.infra_blockers)
			return -EBUSY;
	}
	if (mode == 1 && READ_ONCE(spm->timer_ms) > 20000)
		return -ERANGE;
	/* Keep a 32 kHz time base for external wakes too. The maximum timer
	 * supplies a rollover wake after about 36 hours, not a short timeout.
	 */
	if (spm->sleep_clock_ready && !ticks)
		ticks = U32_MAX;
	spm->wake.flags_expected = PCM_RETAIN_FLAGS;
	if (mode == 2)
		spm->wake.flags_expected &= ~BIT(0);
	if (spm->wake.ddrphy_requested)
		spm->wake.flags_expected &= ~BIT(2); /* BSP DDRPHY_PDN_DIS. */
	if (spm->wake.infra_requested)
		spm->wake.flags_expected &= ~BIT(1); /* BSP INFRA_PDN_DIS. */
	spm->wake.power_before = readl(spm->base + SPM_PWR_STATUS);
	spm->wake.power2_before = readl(spm->base + SPM_PWR_STATUS2);
	/* Synchronize internal registers before connecting power control.
	 * R0 is normally disconnected, retaining firmware crystal control.
	 */
	writel(readl(spm->base + 0x010), spm->base + PCM_DATA_INI);
	writel(BIT(16), spm->base + PCM_PWR_IO_EN);
	writel(0, spm->base + PCM_PWR_IO_EN);
	writel(readl(spm->base + 0x014), spm->base + PCM_DATA_INI);
	writel(BIT(23), spm->base + PCM_PWR_IO_EN);
	writel(0, spm->base + PCM_PWR_IO_EN);
	writel(0, spm->base + PCM_DATA_INI);
	writel(~0U, spm->base + PCM_MAS_PAUSE_MASK);
	writel(spm->wake.flags_expected, spm->base + PCM_FLAGS);
	writel(0, spm->base + 0xb00);
	mt6580_pcm_source_requests(spm);
	if (spm->wake.r0_requested) {
		if (mode != 2)
			spm->wake.r0_blockers |= BIT(0);
		if (spm->wake.src_blockers)
			spm->wake.r0_blockers |= BIT(1);
		if ((spm->wake.power_before | spm->wake.power2_before) &
		    (BIT(0) | BIT(1) | BIT(3) | BIT(4) | BIT(5)))
			spm->wake.r0_blockers |= BIT(2);
		if (spm->wake.r13_before & (GENMASK(4, 0) | BIT(14) | BIT(15)))
			spm->wake.r0_blockers |= BIT(3);
		if (spm->wake.r0_blockers)
			return -EBUSY;
		/* Prepare R0 with F26M forced before the separate, guarded
		 * APSRC/F26M request handoffs below.
		 */
		spm->wake.src_expected |= PCM_F26M_REQ;
		writel(spm->wake.src_expected, spm->base + PCM_SRC_REQ);
		if (spm->wake.f26m_requested)
			/* MT6580 BSP spm_register_init(), with MD1 already off. */
			writel(readl(spm->base + SPM_CLK_CON) |
			       SPM_SRCLKENA_MASK | SPM_CXO32K_MD1,
			       spm->base + SPM_CLK_CON);
		ret = mt6580_pcm_sysclk_settle(spm);
		if (ret)
			return ret;
		if (spm->wake.apsrc_requested) {
			spm->wake.ddr_check_before = mt6580_ddr_sleep_snapshot(spm->ddr,
							spm->wake.ddr_before);
			if (spm->wake.ddr_check_before)
				return spm->wake.ddr_check_before;
			/* Hand APSRC to hardware with DDRPHY/infra power retained.
			 * F26M stays forced unless its additional handoff is requested.
			 */
			spm->wake.src_expected &= ~PCM_APSRC_REQ;
			if (spm->wake.f26m_requested)
				spm->wake.src_expected &= ~PCM_F26M_REQ;
			writel(spm->wake.src_expected, spm->base + PCM_SRC_REQ);
		}
		spm->wake.pwr_io_expected |= PCM_R0_OUTPUT;
	}
	if (mode == 2 && spm->wake.src_blockers)
		return -EBUSY;
	writel(readl(spm->base + SPM_CLK_CON) | BIT(5), spm->base + SPM_CLK_CON);
	spm->wake.clk_expected = spm->wake.clk_con | BIT(5);
	if (spm->wake.r0_requested)
		spm->wake.clk_expected |= SPM_SYSSETTLE_SEL;
	if (spm->wake.f26m_requested)
		spm->wake.clk_expected |= SPM_SRCLKENA_MASK | SPM_CXO32K_MD1;
	writel((readl(spm->base + SPM_AP_STANDBY) & ~GENMASK(7, 4)) | BIT(4) | BIT(6),
	       spm->base + SPM_AP_STANDBY);
	/* Only CPU0 participates: CPU1--3 are already physically offline. */
	for (i = 0; i < 8; i++)
		writel(!i, spm->base + 0xf00 + i * 4);
	writel(~PCM_WAKE_SOURCES, spm->base + SPM_WAKE_MASK);
	writel(PCM_RETURN, spm->base + SPM_ISR_STATUS);
	writel(1, spm->base + PCM_SW_INT_CLEAR);
	writel((readl(spm->base + SPM_ISR_MASK) & BIT(2)) | GENMASK(15, 9),
	       spm->base + SPM_ISR_MASK);
	writel(ticks, spm->base + PCM_TIMER_VAL);
	con1 = readl(spm->base + PCM_CON1) & ~PCM_TIMER_EN;
	if (ticks)
		con1 |= PCM_TIMER_EN;
	writel(PCM_KEY | con1, spm->base + PCM_CON1);
	spm->wake.timer_con_start = readl(spm->base + PCM_CON1);
	spm->wake.timer_val_start = readl(spm->base + PCM_TIMER_VAL);
	if (readl(spm->base + PCM_FLAGS) != spm->wake.flags_expected ||
	    readl(spm->base + PCM_SRC_REQ) != spm->wake.src_expected)
		return -EIO;
	if (spm->wake.axi26_requested) {
		spm->axi.mainpll_requested = spm->wake.mainpll_requested;
		ret = mt6580_axi_sleep_prepare(spm->clock_ctl[0], spm->clock_ctl[1],
					      &spm->axi);
		if (ret)
			return ret;
	}
	writel(spm->wake.pwr_io_expected, spm->base + PCM_PWR_IO_EN);
	if (mode)
		ret = mt6580_cpu0_suspend(&spm->cpu0, mode, mt6580_pcm_idle_finish,
					 mt6580_pcm_wait_finish, (unsigned long)spm);
	else
		ret = mt6580_pcm_idle_finish((unsigned long)spm);
	/* Also clean up a notifier/finisher abort before wait_finish ran. */
	if (spm->wake.axi26_requested) {
		if (!spm->wake.pcm_started ||
		    (readl(spm->base + SPM_ISR_STATUS) & PCM_RETURN))
			restore = mt6580_axi_sleep_resume(spm->clock_ctl[0],
							 spm->clock_ctl[1], &spm->axi);
		else
			restore = -ETIMEDOUT;
		if (!ret)
			ret = restore;
		if (!ret)
			ret = spm->axi.error;
	}
	/* A PLL still owned/off cannot be treated as a recoverable suspend
	 * failure: device resume and CPU DVFS may immediately consume it.
	 * Keep AXI on 26M and stop before the normal device-resume path.
	 */
	if (spm->axi.mainpll_off)
		panic("MT6580: MAINPLL recovery failed (%d), AXI retained at 26M", ret);
	if (spm->axi.emi_paused)
		panic("MT6580: EMI DCM recovery failed (%d), AXI retained at 26M", ret);
	mt6580_pcm_account_time(spm);
	if (ret)
		return ret;
	ret = mt6580_pcm_wait_finish((unsigned long)spm);
	capture = mt6580_pcm_capture(spm);
	if (spm->wake.apsrc_requested) {
		spm->wake.ddr_check_after = mt6580_ddr_sleep_snapshot(spm->ddr,
							spm->wake.ddr_after);
		if (!ret)
			ret = spm->wake.ddr_check_after;
	}
	if (!ret)
		ret = capture;
	if (spm->wake.irq_pending && !spm->wake.forced_exit)
		pm_system_irq_wakeup(spm->irq);
	if (!ret && (spm->wake.assert_pc || spm->wake.r11 != 0x55aa55aa ||
		     spm->wake.power_before != spm->wake.power_after ||
		     spm->wake.power2_before != spm->wake.power2_after ||
		     spm->wake.pwr_io != spm->wake.pwr_io_expected ||
		     spm->wake.flags != spm->wake.flags_expected ||
		     spm->wake.src_req != spm->wake.src_expected ||
		     spm->wake.clk_wake != spm->wake.clk_expected ||
		     (spm->wake.ddrphy_requested &&
		      spm->wake.dpy_after != spm->wake.dpy_before) ||
		     (spm->wake.infra_requested &&
		      (spm->wake.ifr_after & GENMASK(4, 0)) != 0x0d) ||
		     (spm->wake.r0_requested &&
		      (spm->wake.r0 != spm->wake.poweron0 ||
		       spm->wake.settle_wake != spm->wake.settle_applied))))
		ret = -EIO;
	return ret;
}

static irqreturn_t mt6580_spm_irq(int irq, void *data)
{
	struct mt6580_spm *spm = data;
	u32 isr = readl(spm->base + SPM_ISR_STATUS);

	if (!(isr & (PCM_RETURN | PCM_SW_INT0)))
		return IRQ_NONE;
	spm->irqs++;
	spm->irq_status = isr;
	writel(readl(spm->base + SPM_ISR_MASK) | PCM_IRQ_MASK, spm->base + SPM_ISR_MASK);
	writel(PCM_RETURN, spm->base + SPM_ISR_STATUS);
	writel(1, spm->base + PCM_SW_INT_CLEAR);
	return IRQ_HANDLED;
}

static int mt6580_spm_prepare_late(void)
{
	struct mt6580_spm *spm = mt6580_spm;
	enum mt6580_vproc_sleep_mode bank_test = MT6580_VPROC_RETAIN;
	int ret = mt6580_pcm_idle(spm);

	if (spm->requested_state == PM_SUSPEND_STANDBY)
		bank_test = READ_ONCE(spm->voltage_bank_control);
	else if (spm->requested_state == PM_SUSPEND_MEM && spm->vproc_retention)
		bank_test = MT6580_VPROC_RETENTION_AUTO;
	if (spm->requested_state == PM_SUSPEND_STANDBY && bank_test &&
	    (READ_ONCE(spm->cpu0_mode) != 2 || !READ_ONCE(spm->r0_control) ||
	     !READ_ONCE(spm->apsrc_control) || !READ_ONCE(spm->f26m_control) ||
	     !READ_ONCE(spm->ddrphy_control) || !READ_ONCE(spm->infra_control) ||
	     !READ_ONCE(spm->axi_26m_control) || !READ_ONCE(spm->mainpll_control)))
		return -EINVAL;
	spm->sleep_clock_ready = !mt6580_suspend_clock_prepare();
	if (!ret && !spm->sleep_clock_ready && READ_ONCE(spm->f26m_control))
		ret = -ENODEV;
	if (!ret)
		ret = mt6580_pwrap_prepare_suspend(spm->pwrap, bank_test);
	if (!ret) {
		ret = enable_irq_wake(spm->irq);
		if (!ret)
			spm->irq_wake = true;
	}
	spm->last_error = ret;
	return ret;
}

static void mt6580_spm_finish(void)
{
	struct mt6580_spm *spm = mt6580_spm;
	int ret = mt6580_pwrap_finish_suspend(spm->pwrap);

	if (spm->irq_wake) {
		disable_irq_wake(spm->irq);
		spm->irq_wake = false;
	}
	if (ret) {
		spm->last_error = ret;
		dev_err(spm->dev, "PWRAP suspend recovery/link check failed: %d\n", ret);
	}
}

static int mt6580_spm_enter(suspend_state_t state)
{
	struct mt6580_spm *spm = mt6580_spm;
	enum mt6580_pcm_profile profile;
	int i, ret, restore;

	if ((state != PM_SUSPEND_STANDBY && state != PM_SUSPEND_MEM) ||
	    num_online_cpus() != 1 ||
	    smp_processor_id())
		return -EINVAL;
	/* An incomplete transaction owns the parked source until recovery. */
	if (spm->axi.parked || spm->axi.mainpll_off) {
		ret = -EBUSY;
		goto out;
	}
	spm->last_state = state;
	spm->deep_fallback = false;
	spm->fallback_source = 0;
	spm->fallback_r0 = 0;
	spm->fallback_infra = 0;
	profile = state == PM_SUSPEND_MEM ? MT6580_PCM_DEEP : MT6580_PCM_QUALIFY;
	if (state == PM_SUSPEND_MEM)
		spm->deep_requests++;
retry:
	ret = mt6580_pcm_idle(spm);
	if (ret)
		goto out;
	memset(&spm->wake, 0, sizeof(spm->wake));
	spm->wake.settle_before = readl(spm->base + SPM_CLK_SETTLE);
	/* Genpd/device suspend and secondary CPU power-off have completed.
	 * Both automatic deep and explicit qualification validate handoffs
	 * before allowing PCM to control power/clock signals.
	 */
	for (i = 0; i < ARRAY_SIZE(pcm_saved_regs); i++)
		spm->saved[i] = readl(spm->base + pcm_saved_regs[i]);
	ret = mt6580_pcm_load(spm);
	if (!ret)
		ret = mt6580_pcm_run(spm, profile);
	restore = mt6580_pcm_restore(spm);
	/* A busy supplier is a normal automatic-suspend condition. Only fall
	 * back after a clean rollback and before any CPU/WFI entry. Explicit
	 * qualification retains strict errors, and actual resume errors must
	 * never be hidden by trying another suspend.
	 */
	if (profile == MT6580_PCM_DEEP && ret == -EBUSY && !restore &&
	    !spm->wake.time_started) {
		spm->deep_fallback = true;
		spm->deep_fallbacks++;
		spm->fallback_source = spm->wake.src_blockers;
		spm->fallback_r0 = spm->wake.r0_blockers;
		spm->fallback_infra = spm->wake.infra_blockers;
		profile = MT6580_PCM_RETAIN;
		goto retry;
	}
	if (!ret)
		ret = restore;
out:
	spm->last_error = ret;
	return ret;
}

static int mt6580_spm_valid(suspend_state_t state)
{
	return state == PM_SUSPEND_STANDBY || state == PM_SUSPEND_MEM;
}

static int mt6580_spm_begin(suspend_state_t state)
{
	WRITE_ONCE(mt6580_spm->suspend_active, true);
	mt6580_spm->requested_state = state;
	return 0;
}

static void mt6580_spm_end(void)
{
	WRITE_ONCE(mt6580_spm->suspend_active, false);
}

static const struct platform_suspend_ops mt6580_spm_ops = {
	.valid = mt6580_spm_valid,
	.begin = mt6580_spm_begin,
	.end = mt6580_spm_end,
	.prepare_late = mt6580_spm_prepare_late,
	.enter = mt6580_spm_enter,
	.finish = mt6580_spm_finish,
};

static ssize_t state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mt6580_spm *spm = dev_get_drvdata(dev);

	ssize_t len;

	len = sysfs_emit(buf, "mode=retained-run pcm_words=%zu dma=%pad\n"
			 "entry state=%u cpu0_mode=%u fallback=%u deep_requests=%u deep_fallbacks=%u\n"
			 "fallback source=%08x r0=%08x infra=%08x\n"
			 "axi26 requested=%u prepared=%u parked=%u entries=%u resumes=%u error=%d\n"
			 "axi_mux=%08x/%08x/%08x/%08x cpu_mux=%08x/%08x\n"
			 "mainpll con0=%08x/%08x con1=%08x/%08x con2=%08x/%08x\n"
			 "mainpll con3=%08x/%08x power=%08x/%08x\n"
			 "loads=%u last_error=%d loaded_fsm=%08x restored_fsm=%08x\n"
			 "fsm=%08x con0=%08x con1=%08x pwr_io=%08x\n"
			 "runs=%u irqs=%u irq=%d irq_status=%08x pending=%u forced_exit=%u timer_ms=%u\n"
			 "wake r11=%08x r12=%08x r13=%08x raw=%08x misc=%08x timer=%u\n"
			 "isr=%08x event=%08x pc=%08x assert=%08x debug=%08x dvfs=%08x\n"
			 "power=%08x/%08x power2=%08x/%08x\n"
			 "timer_cfg=%08x/%08x val=%u/%u at_wake=%u sleep=%08x wake_fsm=%08x\n"
			 "wake_pwr_io=%08x flags=%08x src_req=%08x\n"
			 "source before=%08x applied=%08x blockers=%08x r13_before=%08x usb_ready=%08x\n"
			 "source poweron=%08x/%08x clk_con=%08x standby=%08x mmddr=%08x\n"
			 "r0 requested=%u value=%08x baseline=%08x blockers=%08x\n"
			 "sysclk_settle before=%u md=%u programmed=%08x applied=%u wake=%u restored=%u\n"
			 "apsrc requested=%u ddr_check=%d/%d\n"
			 "f26m requested=%u clk_con expected=%08x wake=%08x restored=%08x\n"
			 "ddrphy requested=%u power=%08x/%08x\n"
			 "infra requested=%u blockers=%08x power=%08x/%08x sram=%08x/%08x\n"
			 "sleep_clock ready=%u ticks=%u/%u counter=%llu/%llu stopped_ns=%llu\n"
			 "ddr before=%08x/%08x/%08x/%08x after=%08x/%08x/%08x/%08x\n"
			 "cpu0 mode=%u resumes=%u result=%d vector=%08x/%08x/%08x\n"
			 "cpu0 timer_ctrl=%08x/%08x counter=%llu/%llu\n"
			 "cpu0 cold=%u aborts=%u errata=%u l2=%08x/%08x cache=%08x/%08x\n"
			 "cpu0 watchdog=%08x/%08x/%08x length=%08x\n",
			 ARRAY_SIZE(mt6580_suspend_pcm), &spm->pcm_dma,
			 spm->last_state, spm->wake.cpu0_mode, spm->deep_fallback,
			 spm->deep_requests, spm->deep_fallbacks,
			 spm->fallback_source, spm->fallback_r0, spm->fallback_infra,
			 spm->wake.axi26_requested, spm->axi.prepared, spm->axi.parked,
			 spm->axi.entries, spm->axi.resumes, spm->axi.error,
			 spm->axi.mux_before, spm->axi.mux_parked,
			 spm->axi.mux_wake, spm->axi.mux_after,
			 spm->axi.cpu_mux_before, spm->axi.cpu_mux_after,
			 spm->axi.pll_before[0], spm->axi.pll_after[0],
			 spm->axi.pll_before[1], spm->axi.pll_after[1],
			 spm->axi.pll_before[2], spm->axi.pll_after[2],
			 spm->axi.pll_before[3], spm->axi.pll_after[3],
			 spm->axi.pll_before[4], spm->axi.pll_after[4],
			 spm->loads, spm->last_error, spm->loaded_fsm, spm->restored_fsm,
			 readl(spm->base + PCM_FSM_STA), readl(spm->base + PCM_CON0),
			 readl(spm->base + PCM_CON1), readl(spm->base + PCM_PWR_IO_EN),
			 spm->runs, spm->irqs, spm->irq, spm->irq_status,
			 spm->wake.irq_pending, spm->wake.forced_exit, READ_ONCE(spm->timer_ms),
			 spm->wake.r11, spm->wake.r12, spm->wake.r13, spm->wake.raw,
			 spm->wake.misc, spm->wake.timer, spm->wake.isr, spm->wake.event,
			 spm->wake.pc, spm->wake.assert_pc, spm->wake.debug, spm->wake.dvfs,
			 spm->wake.power_before, spm->wake.power_after,
			 spm->wake.power2_before, spm->wake.power2_after,
			 spm->wake.timer_con_start, spm->wake.timer_con_wake,
			 spm->wake.timer_val_start, spm->wake.timer_val_wake,
			 spm->wake.timer_at_wake, spm->wake.sleep_sta, spm->wake.fsm_at_wake,
			 spm->wake.pwr_io, spm->wake.flags, spm->wake.src_req,
			 spm->wake.src_before, spm->wake.src_expected, spm->wake.src_blockers,
			 spm->wake.r13_before, spm->wake.usb_ready,
			 spm->wake.poweron0, spm->wake.poweron1,
			 spm->wake.clk_con, spm->wake.standby, spm->wake.mmddr,
			 spm->wake.r0_requested, spm->wake.r0, spm->wake.poweron0,
			 spm->wake.r0_blockers,
			 spm->wake.settle_before, spm->wake.settle_md, spm->wake.settle_programmed,
			 spm->wake.settle_applied, spm->wake.settle_wake,
			 spm->wake.settle_restored,
			 spm->wake.apsrc_requested, spm->wake.ddr_check_before,
			 spm->wake.ddr_check_after,
			 spm->wake.f26m_requested, spm->wake.clk_expected,
			 spm->wake.clk_wake, spm->wake.clk_restored,
			 spm->wake.ddrphy_requested, spm->wake.dpy_before, spm->wake.dpy_after,
			 spm->wake.infra_requested, spm->wake.infra_blockers,
			 spm->wake.ifr_before, spm->wake.ifr_after,
			 spm->wake.ifrsram_before, spm->wake.ifrsram_after,
			 spm->sleep_clock_ready,
			 spm->wake.time_ticks_start, spm->wake.time_ticks_end,
			 spm->wake.time_counter_start, spm->wake.time_counter_end,
			 spm->wake.stopped_ns,
			 spm->wake.ddr_before[0], spm->wake.ddr_before[1],
			 spm->wake.ddr_before[2], spm->wake.ddr_before[3],
			 spm->wake.ddr_after[0], spm->wake.ddr_after[1],
			 spm->wake.ddr_after[2], spm->wake.ddr_after[3],
			 READ_ONCE(spm->cpu0_mode), spm->cpu0.resumes, spm->cpu0.result,
			 spm->cpu0.boot_before, spm->cpu0.boot_resume, spm->cpu0.boot_after,
			 spm->cpu0.timer_ctrl, spm->cpu0.timer_ctrl_after,
			 spm->cpu0.counter_before, spm->cpu0.counter_after, spm->cpu0.cold_resumes,
			 spm->cpu0.aborts, spm->cpu0.errata_runs, spm->cpu0.l2ctlr,
			 spm->cpu0.l2ctlr_after, spm->cpu0.cache_config, spm->cpu0.cache_after,
			 spm->cpu0.wdt_before, spm->cpu0.wdt_armed, spm->cpu0.wdt_after,
			 spm->cpu0.wdt_length);
	len += sysfs_emit_at(buf, len,
			 "mainpll requested=%u off=%u power_downs=%u power_ups=%u\n"
			 "mainpll_restore automatic=%u software_enable=%u\n"
			 "mainpll_off=%08x/%08x/%08x/%08x/%08x\n"
			 "mainpll_wake=%08x/%08x/%08x/%08x/%08x\n",
			 spm->wake.mainpll_requested, spm->axi.mainpll_off,
			 spm->axi.power_downs, spm->axi.power_ups,
			 spm->axi.mainpll_auto_restored, spm->axi.mainpll_enable_written,
			 spm->axi.pll_off[0], spm->axi.pll_off[1], spm->axi.pll_off[2],
			 spm->axi.pll_off[3], spm->axi.pll_off[4],
			 spm->axi.pll_wake[0], spm->axi.pll_wake[1], spm->axi.pll_wake[2],
			 spm->axi.pll_wake[3], spm->axi.pll_wake[4]);
	len += sysfs_emit_at(buf, len,
			 "dcm active=%u paused=%u pauses=%u restores=%u\n"
			 "dcm_before=%08x/%08x/%08x/%08x/%08x/%08x\n"
			 "dcm_after=%08x/%08x/%08x/%08x/%08x/%08x\n",
			 spm->axi.dcm_active, spm->axi.dcm_paused,
			 spm->axi.dcm_pauses, spm->axi.dcm_restores,
			 spm->axi.dcm_before[0], spm->axi.dcm_before[1],
			 spm->axi.dcm_before[2], spm->axi.dcm_before[3],
			 spm->axi.dcm_before[4], spm->axi.dcm_before[5],
			 spm->axi.dcm_after[0], spm->axi.dcm_after[1],
			 spm->axi.dcm_after[2], spm->axi.dcm_after[3],
			 spm->axi.dcm_after[4], spm->axi.dcm_after[5]);
	return len + sysfs_emit_at(buf, len,
			 "emi active=%u paused=%u pauses=%u restores=%u\n"
			 "emi_before=%08x/%08x after=%08x/%08x\n",
			 spm->axi.emi_active, spm->axi.emi_paused,
			 spm->axi.emi_pauses, spm->axi.emi_restores,
			 spm->axi.emi_before[0], spm->axi.emi_before[1],
			 spm->axi.emi_after[0], spm->axi.emi_after[1]);
}
static DEVICE_ATTR_RO(state);

static ssize_t pcm_timer_ms_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mt6580_spm *spm = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(spm->timer_ms));
}

static ssize_t pcm_timer_ms_store(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct mt6580_spm *spm = dev_get_drvdata(dev);
	unsigned int ms;
	int ret = kstrtouint(buf, 0, &ms);

	if (ret)
		return ret;
	if (ms > 60000)
		return -ERANGE;
	WRITE_ONCE(spm->timer_ms, ms);
	return count;
}
static DEVICE_ATTR_RW(pcm_timer_ms);

static ssize_t cpu0_mode_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mt6580_spm *spm = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(spm->cpu0_mode));
}

static ssize_t cpu0_mode_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct mt6580_spm *spm = dev_get_drvdata(dev);
	unsigned int mode;
	int ret = kstrtouint(buf, 0, &mode);

	if (ret)
		return ret;
	if (mode > 2)
		return -ERANGE;
	WRITE_ONCE(spm->cpu0_mode, mode);
	return count;
}
static DEVICE_ATTR_RW(cpu0_mode);

static ssize_t r0_control_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mt6580_spm *spm = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(spm->r0_control));
}

static ssize_t r0_control_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct mt6580_spm *spm = dev_get_drvdata(dev);
	bool enable;
	int ret = kstrtobool(buf, &enable);

	if (ret)
		return ret;
	WRITE_ONCE(spm->r0_control, enable);
	return count;
}
static DEVICE_ATTR_RW(r0_control);

static ssize_t apsrc_control_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mt6580_spm *spm = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(spm->apsrc_control));
}

static ssize_t apsrc_control_store(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct mt6580_spm *spm = dev_get_drvdata(dev);
	bool enable;
	int ret = kstrtobool(buf, &enable);

	if (ret)
		return ret;
	WRITE_ONCE(spm->apsrc_control, enable);
	return count;
}
static DEVICE_ATTR_RW(apsrc_control);

static ssize_t f26m_control_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mt6580_spm *spm = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(spm->f26m_control));
}

static ssize_t f26m_control_store(struct device *dev, struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct mt6580_spm *spm = dev_get_drvdata(dev);
	bool enable;
	int ret = kstrtobool(buf, &enable);

	if (ret)
		return ret;
	WRITE_ONCE(spm->f26m_control, enable);
	return count;
}
static DEVICE_ATTR_RW(f26m_control);

static ssize_t ddrphy_control_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mt6580_spm *spm = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(spm->ddrphy_control));
}

static ssize_t ddrphy_control_store(struct device *dev, struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct mt6580_spm *spm = dev_get_drvdata(dev);
	bool enable;
	int ret = kstrtobool(buf, &enable);

	if (ret)
		return ret;
	WRITE_ONCE(spm->ddrphy_control, enable);
	return count;
}
static DEVICE_ATTR_RW(ddrphy_control);

static ssize_t infra_control_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mt6580_spm *spm = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(spm->infra_control));
}

static ssize_t infra_control_store(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct mt6580_spm *spm = dev_get_drvdata(dev);
	bool enable;
	int ret = kstrtobool(buf, &enable);

	if (ret)
		return ret;
	WRITE_ONCE(spm->infra_control, enable);
	return count;
}
static DEVICE_ATTR_RW(infra_control);

static ssize_t axi_26m_control_show(struct device *dev, struct device_attribute *attr,
				  char *buf)
{
	struct mt6580_spm *spm = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(spm->axi_26m_control));
}

static ssize_t axi_26m_control_store(struct device *dev, struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct mt6580_spm *spm = dev_get_drvdata(dev);
	bool enable;
	int ret = kstrtobool(buf, &enable);

	if (ret)
		return ret;
	if (enable && (!spm->clock_ctl[0] || !spm->clock_ctl[1]))
		return -ENODEV;
	WRITE_ONCE(spm->axi_26m_control, enable);
	return count;
}
static DEVICE_ATTR_RW(axi_26m_control);

static ssize_t mainpll_control_show(struct device *dev, struct device_attribute *attr,
				   char *buf)
{
	struct mt6580_spm *spm = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(spm->mainpll_control));
}

static ssize_t mainpll_control_store(struct device *dev, struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct mt6580_spm *spm = dev_get_drvdata(dev);
	bool enable;
	int ret = kstrtobool(buf, &enable);

	if (ret)
		return ret;
	if (enable && (!spm->clock_ctl[0] || !spm->clock_ctl[1]))
		return -ENODEV;
	WRITE_ONCE(spm->mainpll_control, enable);
	return count;
}
static DEVICE_ATTR_RW(mainpll_control);

static ssize_t voltage_bank_control_show(struct device *dev, struct device_attribute *attr,
					char *buf)
{
	struct mt6580_spm *spm = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", READ_ONCE(spm->voltage_bank_control));
}

static ssize_t voltage_bank_control_store(struct device *dev, struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct mt6580_spm *spm = dev_get_drvdata(dev);
	unsigned int mode;
	int ret = kstrtouint(buf, 0, &mode);

	if (ret)
		return ret;
	if (mode > MT6580_VPROC_RETENTION_TEST)
		return -EINVAL;
	WRITE_ONCE(spm->voltage_bank_control, mode);
	return count;
}
static DEVICE_ATTR_RW(voltage_bank_control);

static struct attribute *mt6580_spm_attrs[] = {
	&dev_attr_state.attr,
	&dev_attr_idle_state.attr,
	&dev_attr_pcm_timer_ms.attr,
	&dev_attr_cpu0_mode.attr,
	&dev_attr_r0_control.attr,
	&dev_attr_apsrc_control.attr,
	&dev_attr_f26m_control.attr,
	&dev_attr_ddrphy_control.attr,
	&dev_attr_infra_control.attr,
	&dev_attr_axi_26m_control.attr,
	&dev_attr_mainpll_control.attr,
	&dev_attr_voltage_bank_control.attr,
	NULL,
};
ATTRIBUTE_GROUPS(mt6580_spm);

static void mt6580_spm_put_device(void *data)
{
	put_device(data);
}

static int mt6580_spm_get_clock_controllers(struct mt6580_spm *spm)
{
	static const char * const compatible[] = {
		"mediatek,mt6580-topckgen", "mediatek,mt6580-apmixedsys",
	};
	struct device *dev = spm->dev;
	struct platform_device *controller;
	struct device_node *node;
	int i, ret;

	if (!of_property_present(dev->of_node, "mediatek,clock-controllers"))
		return 0;
	ret = of_count_phandle_with_args(dev->of_node, "mediatek,clock-controllers", NULL);
	if (ret != ARRAY_SIZE(spm->clock_ctl))
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(spm->clock_ctl); i++) {
		node = of_parse_phandle(dev->of_node, "mediatek,clock-controllers", i);
		if (!of_device_is_compatible(node, compatible[i]) ||
		    !of_device_is_available(node)) {
			of_node_put(node);
			return -EINVAL;
		}
		controller = of_find_device_by_node(node);
		of_node_put(node);
		if (!controller)
			return -EPROBE_DEFER;
		if (!device_is_bound(&controller->dev)) {
			put_device(&controller->dev);
			return -EPROBE_DEFER;
		}
		ret = devm_add_action_or_reset(dev, mt6580_spm_put_device, &controller->dev);
		if (ret)
			return ret;
		if (!device_link_add(dev, &controller->dev, DL_FLAG_AUTOREMOVE_CONSUMER))
			return -EINVAL;
		spm->clock_ctl[i] = &controller->dev;
	}
	return 0;
}

static void mt6580_spm_unmap(void *data)
{
	iounmap(data);
}

static void mt6580_spm_unregister(void *data)
{
	suspend_set_ops(NULL);
	mt6580_spm = NULL;
}

static int mt6580_spm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct platform_device *pwrap;
	struct platform_device *ddr;
	struct platform_device *uart;
	struct platform_device *usb;
	struct device_node *node;
	struct mt6580_spm *spm;
	struct clk_bulk_data *clks;
	int ret, i;

	if (mt6580_spm)
		return -EBUSY;
	if (!of_device_is_compatible(dev->parent->of_node, "mediatek,mt6580-scpsys"))
		return -EINVAL;
	node = of_parse_phandle(dev->of_node, "mediatek,pwrap", 0);
	if (!node)
		return -EINVAL;
	if (!of_device_is_compatible(node, "mediatek,mt6580-firmware-pwrap") &&
	    !of_device_is_compatible(node, "mediatek,mt6580-pwrap")) {
		of_node_put(node);
		return -EINVAL;
	}
	pwrap = of_find_device_by_node(node);
	of_node_put(node);
	if (!pwrap)
		return -EPROBE_DEFER;
	if (!device_is_bound(&pwrap->dev)) {
		put_device(&pwrap->dev);
		return -EPROBE_DEFER;
	}
	ret = devm_add_action_or_reset(dev, mt6580_spm_put_device, &pwrap->dev);
	if (ret)
		return ret;
	if (!device_link_add(dev, &pwrap->dev, DL_FLAG_AUTOREMOVE_CONSUMER))
		return -EINVAL;
	spm = devm_kzalloc(dev, sizeof(*spm), GFP_KERNEL);
	if (!spm)
		return -ENOMEM;
	spm->dev = dev;
	spm->vproc_retention = of_property_read_bool(dev->of_node, "mediatek,vproc-retention");
	spm->pwrap = &pwrap->dev;
	ret = mt6580_spm_get_clock_controllers(spm);
	if (ret)
		return ret;
	node = of_parse_phandle(dev->of_node, "mediatek,ddr", 0);
	if (!of_device_is_compatible(node, "mediatek,mt6580-ddrphy-clock") ||
	    !of_device_is_available(node)) {
		of_node_put(node);
		return -EINVAL;
	}
	ddr = of_find_device_by_node(node);
	of_node_put(node);
	if (!ddr)
		return -EPROBE_DEFER;
	if (!device_is_bound(&ddr->dev)) {
		put_device(&ddr->dev);
		return -EPROBE_DEFER;
	}
	ret = devm_add_action_or_reset(dev, mt6580_spm_put_device, &ddr->dev);
	if (ret)
		return ret;
	if (!device_link_add(dev, &ddr->dev, DL_FLAG_AUTOREMOVE_CONSUMER))
		return -EINVAL;
	spm->ddr = &ddr->dev;
	ret = of_count_phandle_with_args(dev->of_node, "mediatek,uarts", NULL);
	if (ret < 1 || ret > ARRAY_SIZE(spm->uarts))
		return -EINVAL;
	spm->num_uarts = ret;
	for (i = 0; i < spm->num_uarts; i++) {
		node = of_parse_phandle(dev->of_node, "mediatek,uarts", i);
		if (!of_device_is_compatible(node, "mediatek,mt6580-uart") ||
		    !of_device_is_available(node)) {
			of_node_put(node);
			return -EINVAL;
		}
		uart = of_find_device_by_node(node);
		of_node_put(node);
		if (!uart)
			return -EPROBE_DEFER;
		if (!device_is_bound(&uart->dev)) {
			put_device(&uart->dev);
			return -EPROBE_DEFER;
		}
		ret = devm_add_action_or_reset(dev, mt6580_spm_put_device, &uart->dev);
		if (ret)
			return ret;
		if (!device_link_add(dev, &uart->dev, DL_FLAG_AUTOREMOVE_CONSUMER))
			return -EINVAL;
		spm->uarts[i] = &uart->dev;
	}
	/* A board may not omit an enabled UART from the handoff check. */
	for_each_compatible_node(node, NULL, "mediatek,mt6580-uart") {
		if (!of_device_is_available(node))
			continue;
		for (i = 0; i < spm->num_uarts; i++)
			if (spm->uarts[i]->of_node == node)
				break;
		if (i == spm->num_uarts) {
			of_node_put(node);
			return -EINVAL;
		}
	}
	ret = of_count_phandle_with_args(dev->of_node, "mediatek,usbs", NULL);
	if (ret < 1 || ret > ARRAY_SIZE(spm->usbs))
		return -EINVAL;
	spm->num_usbs = ret;
	for (i = 0; i < spm->num_usbs; i++) {
		node = of_parse_phandle(dev->of_node, "mediatek,usbs", i);
		if (!of_device_is_compatible(node, "mediatek,mt6580-musb") ||
		    !of_device_is_available(node)) {
			of_node_put(node);
			return -EINVAL;
		}
		usb = of_find_device_by_node(node);
		of_node_put(node);
		if (!usb)
			return -EPROBE_DEFER;
		if (!device_is_bound(&usb->dev)) {
			put_device(&usb->dev);
			return -EPROBE_DEFER;
		}
		ret = devm_add_action_or_reset(dev, mt6580_spm_put_device, &usb->dev);
		if (ret)
			return ret;
		if (!device_link_add(dev, &usb->dev, DL_FLAG_AUTOREMOVE_CONSUMER))
			return -EINVAL;
		spm->usbs[i] = &usb->dev;
	}
	/* An enabled USB controller cannot be omitted from the clock audit. */
	for_each_compatible_node(node, NULL, "mediatek,mt6580-musb") {
		if (!of_device_is_available(node))
			continue;
		for (i = 0; i < spm->num_usbs; i++)
			if (spm->usbs[i]->of_node == node)
				break;
		if (i == spm->num_usbs) {
			of_node_put(node);
			return -EINVAL;
		}
	}
	ret = devm_clk_bulk_get_all_enabled(dev, &clks);
	if (ret < 0)
		return dev_err_probe(dev, ret, "SPM clocks unavailable\n");
	/* SCPSYS owns the resource. This child shares only its PCM sub-block. */
	spm->base = of_iomap(dev->parent->of_node, 0);
	if (!spm->base)
		return -ENOMEM;
	ret = devm_add_action_or_reset(dev, mt6580_spm_unmap, spm->base);
	if (ret)
		return ret;
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;
	spm->pcm = dmam_alloc_coherent(dev, sizeof(mt6580_suspend_pcm),
				       &spm->pcm_dma, GFP_KERNEL);
	if (!spm->pcm)
		return -ENOMEM;
	memcpy(spm->pcm, mt6580_suspend_pcm, sizeof(mt6580_suspend_pcm));
	spm->idle.pcm = dmam_alloc_coherent(dev, sizeof(mt6580_idle_pcm),
					    &spm->idle.dma, GFP_KERNEL);
	if (!spm->idle.pcm)
		return -ENOMEM;
	memcpy(spm->idle.pcm, mt6580_idle_pcm, sizeof(mt6580_idle_pcm));
	spm->irq = platform_get_irq(pdev, 0);
	if (spm->irq < 0)
		return spm->irq;
	ret = devm_request_irq(dev, spm->irq, mt6580_spm_irq, 0, dev_name(dev), spm);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, spm);
	mt6580_spm = spm;
	suspend_set_ops(&mt6580_spm_ops);
	ret = devm_add_action_or_reset(dev, mt6580_spm_unregister, spm);
	if (ret)
		return ret;
	dev_info(dev, "PCM %s: deep suspend with guarded retention fallback\n",
		 MT6580_PCM_VERSION);
	return 0;
}

static const struct of_device_id mt6580_spm_match[] = {
	{ .compatible = "mediatek,mt6580-spm-suspend" },
	{ }
};

static struct platform_driver mt6580_spm_driver = {
	.probe = mt6580_spm_probe,
	.driver = {
		.name = "mt6580-spm-suspend",
		.of_match_table = mt6580_spm_match,
		.dev_groups = mt6580_spm_groups,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(mt6580_spm_driver);
