// SPDX-License-Identifier: GPL-2.0-only
/* MT6580 retained-power PCM execution and PWRAP suspend handoff. */
#include <linux/dma-mapping.h>
#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/irq.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/smp.h>
#include <linux/soc/mediatek/mt6580-pwrap.h>
#include <linux/soc/mediatek/mt6580-uart.h>
#include <linux/suspend.h>

#include <asm/proc-fns.h>

#include "mt6580-spm-pcm.h"

#define PCM_CON0		0x310
#define PCM_CON1		0x314
#define PCM_IM_PTR		0x318
#define PCM_IM_LEN		0x31c
#define PCM_PWR_IO_EN		0x358
#define PCM_DATA_INI		0x320
#define PCM_MAS_PAUSE_MASK	0x354
#define PCM_TIMER_VAL		0x35c
#define PCM_TIMER_OUT		0x360
#define PCM_REG11		0x3ac
#define PCM_REG12		0x3b0
#define PCM_REG13		0x3b4
#define PCM_PC			0x3bc
#define PCM_EVENT_STA		0x3c0
#define PCM_FSM_STA		0x3c4
#define PCM_SW_INT_CLEAR	0x3e4
#define SPM_CLK_CON		0x400
#define SPM_POWER_ON_VAL0	0x010
#define SPM_POWER_ON_VAL1	0x014
#define SPM_AP_STANDBY		0x608
#define SPM_PWR_STATUS		0x60c
#define SPM_PWR_STATUS2		0x610
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
/* BSP CPU/infra/DDRPHY/VCORE/PASR/DPD/SODI-EMI/MAINPLL/CPU-DVS disable. */
#define PCM_RETAIN_FLAGS	(GENMASK(5, 0) | GENMASK(9, 7))
#define PCM_WAKE_SOURCES	(BIT(0) | BIT(2) | BIT(3) | BIT(5) | BIT(14) | \
				 BIT(15) | BIT(22) | BIT(26))

static const u16 pcm_saved_regs[] = {
	PCM_CON0, PCM_CON1, PCM_IM_PTR, PCM_IM_LEN,
	0x340, 0x344, 0x348, 0x34c,
	PCM_DATA_INI, PCM_MAS_PAUSE_MASK, PCM_TIMER_VAL, SPM_CLK_CON,
	SPM_AP_STANDBY, SPM_WAKE_MASK, SPM_CPU_WAKE, SPM_ISR_MASK,
	PCM_FLAGS, PCM_SRC_REQ, 0xb00, 0xb04, PCM_DEBUG_FLAG,
	0xb60, 0xb64, 0xb68, 0xb6c,
	0xf00, 0xf04, 0xf08, 0xf0c, 0xf10, 0xf14, 0xf18, 0xf1c,
};

struct mt6580_pcm_wake {
	u32 r11, r12, r13, raw, misc, timer, isr, event, pc, assert_pc, debug;
	u32 power_before, power_after, power2_before, power2_after, dvfs;
	u32 timer_con_start, timer_val_start, timer_con_wake, timer_val_wake;
	u32 timer_at_wake, sleep_sta, fsm_at_wake;
	u32 pwr_io, flags, src_req;
	u32 src_before, src_expected, src_blockers, r13_before;
	u32 poweron0, poweron1, clk_con, standby, mmddr;
	bool irq_pending, forced_exit;
};

struct mt6580_spm {
	struct device *dev;
	struct device *pwrap;
	struct device *uarts[2];
	unsigned int num_uarts;
	void __iomem *base;
	void *pcm;
	dma_addr_t pcm_dma;
	u32 saved[ARRAY_SIZE(pcm_saved_regs)];
	u32 loaded_fsm, restored_fsm;
	unsigned int loads;
	unsigned int runs, irqs, timer_ms;
	u32 irq_status;
	int irq;
	bool irq_wake;
	struct mt6580_pcm_wake wake;
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

static int mt6580_pcm_load(struct mt6580_spm *spm)
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
	writel(lower_32_bits(spm->pcm_dma), spm->base + PCM_IM_PTR);
	writel(ARRAY_SIZE(mt6580_suspend_pcm) - 1, spm->base + PCM_IM_LEN);
	/* Coherent DMA buffer must be visible before starting instruction fetch. */
	dma_wmb();
	writel(PCM_KEY | PCM_IM_SLEEP_DVS | PCM_IM_KICK, spm->base + PCM_CON0);
	writel(PCM_KEY | PCM_IM_SLEEP_DVS, spm->base + PCM_CON0);
	ret = readl_poll_timeout_atomic(spm->base + PCM_FSM_STA, val,
			(val & PCM_IM_STATE_MASK) == PCM_IM_READY, 1, 10000);
	spm->loaded_fsm = val;
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(mt6580_pcm_vectors); i++)
		writel(mt6580_pcm_vectors[i], spm->base + 0x340 + i * 4);
	if (readl(spm->base + PCM_PWR_IO_EN))
		return -EIO;
	spm->loads++;
	return 0;
}

static int mt6580_pcm_restore(struct mt6580_spm *spm)
{
	int i, ret, irq_ret;
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
	irq_ret = irq_set_irqchip_state(spm->irq, IRQCHIP_STATE_PENDING, false);
	ret = mt6580_pcm_reset(spm);
	if (ret)
		return ret;
	/* Restore data/vector registers first and controls last. Never replay a
	 * fetch/run/reset command from a sampled control register.
	 */
	for (i = 2; i < ARRAY_SIZE(pcm_saved_regs); i++)
		writel(spm->saved[i], spm->base + pcm_saved_regs[i]);
	value = spm->saved[1] & 0xffff;
	writel(PCM_KEY | value, spm->base + PCM_CON1);
	value = spm->saved[0] & 0xffff & ~(BIT(0) | PCM_IM_KICK | PCM_SW_RESET);
	writel(PCM_KEY | value, spm->base + PCM_CON0);
	spm->restored_fsm = readl(spm->base + PCM_FSM_STA);
	ret = mt6580_pcm_idle(spm);
	return ret ? ret : irq_ret;
}

static int mt6580_pcm_capture(struct mt6580_spm *spm)
{
	struct mt6580_pcm_wake *wake = &spm->wake;

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

static int mt6580_pcm_run(struct mt6580_spm *spm)
{
	u32 val, con0, con1, ticks = READ_ONCE(spm->timer_ms) * 32768 / 1000;
	int i, ret, capture;

	memset(&spm->wake, 0, sizeof(spm->wake));
	spm->wake.power_before = readl(spm->base + SPM_PWR_STATUS);
	spm->wake.power2_before = readl(spm->base + SPM_PWR_STATUS2);
	/* Synchronize internal registers before connecting R7 control. R0
	 * stays disconnected, retaining the bootloader's crystal control.
	 */
	writel(readl(spm->base + 0x010), spm->base + PCM_DATA_INI);
	writel(BIT(16), spm->base + PCM_PWR_IO_EN);
	writel(0, spm->base + PCM_PWR_IO_EN);
	writel(readl(spm->base + 0x014), spm->base + PCM_DATA_INI);
	writel(BIT(23), spm->base + PCM_PWR_IO_EN);
	writel(0, spm->base + PCM_PWR_IO_EN);
	writel(0, spm->base + PCM_DATA_INI);
	writel(~0U, spm->base + PCM_MAS_PAUSE_MASK);
	writel(PCM_RETAIN_FLAGS, spm->base + PCM_FLAGS);
	writel(0, spm->base + 0xb00);
	mt6580_pcm_source_requests(spm);
	writel(readl(spm->base + SPM_CLK_CON) | BIT(5), spm->base + SPM_CLK_CON);
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
	if (readl(spm->base + PCM_FLAGS) != PCM_RETAIN_FLAGS ||
	    readl(spm->base + PCM_SRC_REQ) != spm->wake.src_expected)
		return -EIO;
	writel(PCM_R7_OUTPUT, spm->base + PCM_PWR_IO_EN);
	con0 = readl(spm->base + PCM_CON0) & ~(PCM_KICK | PCM_IM_KICK);
	writel(PCM_KEY | con0 | PCM_KICK, spm->base + PCM_CON0);
	writel(PCM_KEY | con0, spm->base + PCM_CON0);
	spm->runs++;
	cpu_do_idle();
	spm->wake.timer_con_wake = readl(spm->base + PCM_CON1);
	spm->wake.timer_val_wake = readl(spm->base + PCM_TIMER_VAL);
	spm->wake.timer_at_wake = readl(spm->base + PCM_TIMER_OUT);
	spm->wake.sleep_sta = readl(spm->base + 0x720);
	spm->wake.fsm_at_wake = readl(spm->base + PCM_FSM_STA);
	/* An external IRQ can wake CPU0 before the PCM return sequence ends. */
	if (!(readl(spm->base + SPM_ISR_STATUS) & PCM_RETURN)) {
		spm->wake.forced_exit = true;
		writel(1, spm->base + SPM_CPU_WAKE);
	}
	ret = readl_poll_timeout_atomic(spm->base + SPM_ISR_STATUS, val,
				       val & PCM_RETURN, 1, 100000);
	capture = mt6580_pcm_capture(spm);
	if (!ret)
		ret = capture;
	if (spm->wake.irq_pending && !spm->wake.forced_exit)
		pm_system_irq_wakeup(spm->irq);
	if (!ret && (spm->wake.assert_pc || spm->wake.r11 != 0x55aa55aa ||
		     spm->wake.power_before != spm->wake.power_after ||
		     spm->wake.power2_before != spm->wake.power2_after ||
		     spm->wake.pwr_io != PCM_R7_OUTPUT ||
		     spm->wake.flags != PCM_RETAIN_FLAGS ||
		     spm->wake.src_req != spm->wake.src_expected))
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
	int ret = mt6580_pcm_idle(spm);

	if (!ret)
		ret = mt6580_pwrap_prepare_suspend(spm->pwrap);
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
		dev_err(spm->dev, "PWRAP suspend slots did not restore: %d\n", ret);
	}
}

static int mt6580_spm_enter(suspend_state_t state)
{
	struct mt6580_spm *spm = mt6580_spm;
	int i, ret, restore;

	if (state != PM_SUSPEND_STANDBY || num_online_cpus() != 1 ||
	    smp_processor_id())
		return -EINVAL;
	ret = mt6580_pcm_idle(spm);
	if (ret)
		goto out;
	/* Genpd/device suspend and secondary CPU power-off have completed.
	 * Only R7 is connected; retained flags/requests keep system power on.
	 */
	for (i = 0; i < ARRAY_SIZE(pcm_saved_regs); i++)
		spm->saved[i] = readl(spm->base + pcm_saved_regs[i]);
	ret = mt6580_pcm_load(spm);
	if (!ret)
		ret = mt6580_pcm_run(spm);
	restore = mt6580_pcm_restore(spm);
	if (!ret)
		ret = restore;
out:
	spm->last_error = ret;
	return ret;
}

static int mt6580_spm_valid(suspend_state_t state)
{
	return state == PM_SUSPEND_STANDBY;
}

static const struct platform_suspend_ops mt6580_spm_ops = {
	.valid = mt6580_spm_valid,
	.prepare_late = mt6580_spm_prepare_late,
	.enter = mt6580_spm_enter,
	.finish = mt6580_spm_finish,
};

static ssize_t state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mt6580_spm *spm = dev_get_drvdata(dev);

	return sysfs_emit(buf, "mode=retained-run pcm_words=%zu dma=%pad\n"
			 "loads=%u last_error=%d loaded_fsm=%08x restored_fsm=%08x\n"
			 "fsm=%08x con0=%08x con1=%08x pwr_io=%08x\n"
			 "runs=%u irqs=%u irq=%d irq_status=%08x pending=%u forced_exit=%u timer_ms=%u\n"
			 "wake r11=%08x r12=%08x r13=%08x raw=%08x misc=%08x timer=%u\n"
			 "isr=%08x event=%08x pc=%08x assert=%08x debug=%08x dvfs=%08x\n"
			 "power=%08x/%08x power2=%08x/%08x\n"
			 "timer_cfg=%08x/%08x val=%u/%u at_wake=%u sleep=%08x wake_fsm=%08x\n"
			 "wake_pwr_io=%08x flags=%08x src_req=%08x\n"
			 "source before=%08x applied=%08x blockers=%08x r13_before=%08x\n"
			 "source poweron=%08x/%08x clk_con=%08x standby=%08x mmddr=%08x\n",
			 ARRAY_SIZE(mt6580_suspend_pcm), &spm->pcm_dma,
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
			 spm->wake.r13_before, spm->wake.poweron0, spm->wake.poweron1,
			 spm->wake.clk_con, spm->wake.standby, spm->wake.mmddr);
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

static struct attribute *mt6580_spm_attrs[] = {
	&dev_attr_state.attr,
	&dev_attr_pcm_timer_ms.attr,
	NULL,
};
ATTRIBUTE_GROUPS(mt6580_spm);

static void mt6580_spm_put_device(void *data)
{
	put_device(data);
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
	struct platform_device *uart;
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
	if (!of_device_is_compatible(node, "mediatek,mt6580-firmware-pwrap")) {
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
	spm->pwrap = &pwrap->dev;
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
	dev_info(dev, "PCM %s: retained-power execution; default s2idle retained\n",
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
