// SPDX-License-Identifier: GPL-2.0-only
/* MT6580 WACS2 access and MT6350 serial-link initialization. */
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mfd/mt6350/registers.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/panic.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <linux/soc/mediatek/mt6580-pwrap.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#define WACS2_CMD	0x9c
#define WACS2_RDATA	0xa0
#define WACS2_VLDCLR	0xa4
#define WACS_FSM	GENMASK(18, 16)
#define WACS_IDLE	0
#define WACS_VALID	6
#define WACS_INIT_DONE	BIT(21)
#define MT6350_DEW_READ_TEST	0x18c
#define MT6350_READ_TEST_VALUE	0x5aa5
#define WACS_SYNC_IDLE	BIT(20)

/* MT6580 pwrap_hal.h register layout, not the MT2701 wrapper layout. */
#define PWRAP_MUX_SEL	0x000
#define PWRAP_WRAP_EN	0x004
#define PWRAP_DIO_EN	0x008
#define PWRAP_SIDLY	0x00c
#define PWRAP_RDDMY	0x018
#define PWRAP_SI_CK_CON	0x01c
#define PWRAP_CSHEXT_WRITE 0x020
#define PWRAP_CSHEXT_READ	0x024
#define PWRAP_CSLEXT_START 0x028
#define PWRAP_CSLEXT_END	0x02c
#define PWRAP_STAUPD_PRD	0x030
#define PWRAP_STAUPD_GRPEN 0x034
#define PWRAP_STAUPD_STA 0x03c
#define PWRAP_WRAP_STA	0x044
#define PWRAP_ARB_EN	0x050
#define PWRAP_MAN_EN	0x05c
#define PWRAP_MAN_CMD	0x060
#define PWRAP_WACS0_EN	0x06c
#define PWRAP_INIT_DONE0	0x070
#define PWRAP_WACS1_EN	0x080
#define PWRAP_INIT_DONE1	0x084
#define PWRAP_WACS2_EN	0x094
#define PWRAP_INIT_DONE2	0x098
#define PWRAP_INT_EN	0x0a8
#define PWRAP_INT_RAW	0x0ac
#define PWRAP_INT_FLG	0x0b0
#define PWRAP_INT_CLR	0x0b4
#define PWRAP_SIG_ERRVAL	0x0c4
#define PWRAP_WDT_FLG	0x0dc
#define PWRAP_IRQ_MASK	0x7ffffffd
#define PWRAP_ERROR_MASK	GENMASK(30, 0)
#define PWRAP_SIG_ADR	0x0b8
#define PWRAP_SIG_MODE	0x0bc
#define PWRAP_CRC_EN	0x0c8
#define PWRAP_TIMER_EN	0x0cc
#define PWRAP_WDT_UNIT	0x0d4
#define PWRAP_WDT_SRC_EN	0x0d8
#define PWRAP_CIPHER_KEY	0x124
#define PWRAP_CIPHER_IV	0x128
#define PWRAP_CIPHER_EN	0x12c
#define PWRAP_CIPHER_RDY	0x130
#define PWRAP_CIPHER_MODE	0x134
#define PWRAP_CIPHER_SWRST 0x138
#define PWRAP_DCM_EN	0x13c
#define PWRAP_DCM_DBC	0x140
#define PWRAP_ADC_CMD_ADR	0x144
#define PWRAP_ADC_CMD	0x148
#define PWRAP_ADC_RDY_ADR	0x14c
#define PWRAP_ADC_DATA1	0x150
#define PWRAP_ADC_DATA2	0x154
#define PWRAP_SWRST	0x180
#define PWRAP_WACS2_ARB	BIT(3)

static bool force_init;
module_param(force_init, bool, 0400);
MODULE_PARM_DESC(force_init, "Reinitialize MT6350 serial link once at probe, before consumers");

enum mt6580_pwrap_wait_stage {
	PWRAP_WAIT_IDLE,
	PWRAP_WAIT_READ,
	PWRAP_WAIT_WRITE,
};

struct mt6580_pwrap_link_stats {
	u32 reads, writes, errors, timeouts, init_lost, stale_valid_clears;
	u32 checks, check_errors, test_value;
	int test_error, last_error;
	u32 last_reg, last_status, last_stage;
	bool last_write;
};

/* PCM's AUXADC workaround uses slots 4/5, not the normal VCORE slot 0. */
#define DVFS_ADR4	0x104
#define DVFS_WDATA4	0x108
#define DVFS_ADR5	0x10c
#define DVFS_WDATA5	0x110
#define AUXADC_START	BIT(4)
#define VPROC_CTRL	0x216
#define VPROC_ON	0x220
#define VPROC_SLEEP	0x222
#define VPROC_ACTUAL	0x224
#define VPROC_TRANS	0x230
#define PMIC_CLK_CTRL	0x120
#define VPROC_SLP	0x212
#define VPROC_SLEW	0x21c
#define VPROC_HW_VOSEL	BIT(1)
#define VPROC_SLEEP_EN	BIT(8)
#define VPROC_SLEEP_SEL	BIT(11)
#define VPROC_SEL_MASK	GENMASK(6, 0)
#define VPROC_FLOOR_SEL	0x48
#define VPROC_MARKER_SEL	0x49
#define VPROC_RETENTION_SEL 0x18
#define PMIC_CLK_MASK	(BIT(0) | BIT(4) | BIT(5))
#define VPROC_SLP_MASK	GENMASK(5, 4)
#define VPROC_SLEW_MASK	(BIT(15) | GENMASK(7, 0))
#define VPROC_TRANS_MASK	(BIT(8) | GENMASK(5, 4) | GENMASK(1, 0))

static const u16 retention_regs[] = { PMIC_CLK_CTRL, VPROC_SLP, VPROC_SLEW, VPROC_TRANS };
static const u16 retention_masks[] = {
	PMIC_CLK_MASK, VPROC_SLP_MASK, VPROC_SLEW_MASK, VPROC_TRANS_MASK,
};

static const u16 suspend_slot_regs[] = {
	DVFS_ADR4, DVFS_WDATA4, DVFS_ADR5, DVFS_WDATA5,
};

/* BSP VPROC: control, software/on/sleep selectors, actual selector,
 * and hardware sleep/transition control. Snapshot only; never change policy.
 */
static const u16 vproc_snapshot_regs[] = { 0x216, 0x21e, 0x220, 0x222, 0x224, 0x230 };

/* Only wrapper/autonomous configuration is preserved here, never pending
 * requests or sampled WACS data. Slots 0..7 must survive an online reset.
 */
static const u16 recovery_regs[] = {
	0x04c, PWRAP_ARB_EN, PWRAP_WACS0_EN, PWRAP_WACS1_EN,
	PWRAP_STAUPD_PRD, PWRAP_STAUPD_GRPEN, PWRAP_WDT_UNIT,
	PWRAP_WDT_SRC_EN, PWRAP_TIMER_EN, PWRAP_DCM_EN, PWRAP_DCM_DBC,
	PWRAP_ADC_CMD_ADR, PWRAP_ADC_CMD, PWRAP_ADC_RDY_ADR,
	PWRAP_ADC_DATA1, PWRAP_ADC_DATA2,
	0x0e4, 0x0e8, 0x0ec, 0x0f0, 0x0f4, 0x0f8,
	0x0fc, 0x100, 0x104, 0x108, 0x10c, 0x110,
	0x114, 0x118, 0x11c, 0x120,
};

struct mt6580_pwrap {
	void __iomem *base;
	struct regmap *regmap;
	struct device *dev, *top;
	struct reset_control *reset;
	struct clk *spi, *ref;
	spinlock_t bus_lock, irq_lock;
	unsigned long bus_flags, last_rebuild, last_recovery;
	u32 recovery_burst;
	u32 recovery_config[ARRAY_SIZE(recovery_regs)];
	bool recovery_config_valid;
	struct delayed_work recovery_work;
	struct wakeup_source *recovery_ws;
	int irq;
	bool online, stopping, recovering, failed, fault_pending, force_rebuild;
	u32 fault_flags;
	u32 irq_count, irq_last, irq_raw, irq_wdt, irq_signature, irq_wacs;
	u32 recovery_attempts, recovery_soft, recovery_rebuilds, recovery_failures;
	u32 recovery_deferred, blocked, recovery_before[6], recovery_after[6];
	int recovery_drain_error;
	int recovery_error, recovery_snapshot_error;
	struct mutex phase_lock;
	spinlock_t stats_lock;
	struct mt6580_pwrap_link_stats link;
	bool initializing, firmware_initialized, cold_initialized;
	u32 si_pass_mask, si_point;
	const char *init_stage;
	u32 saved_slots[ARRAY_SIZE(suspend_slot_regs)];
	u32 suspend_slots[ARRAY_SIZE(suspend_slot_regs)];
	unsigned int saved_auxadc, resume_auxadc, restored_auxadc;
	unsigned int prepared, restored;
	unsigned int vproc_before[ARRAY_SIZE(vproc_snapshot_regs)];
	unsigned int vproc_resume[ARRAY_SIZE(vproc_snapshot_regs)];
	int vproc_before_error, vproc_resume_error;
	unsigned int bank_armed[6], bank_forced[6], bank_released[6], bank_restored[6];
	unsigned int bank_entries, bank_restores, bank_markers;
	bool bank_active;
	int bank_error;
	unsigned int bank_mode;
	unsigned int retention_skips;
	int retention_resume_error;
	unsigned int retention_before[4], retention_armed[4];
	unsigned int retention_resume[4], retention_restored[4];
	bool suspend_phase;
};

static void mt6580_pwrap_request_recovery(struct mt6580_pwrap *wrp);
static ssize_t recovery_state_show(struct device *, struct device_attribute *, char *);
static ssize_t recover_store(struct device *, struct device_attribute *, const char *, size_t);
static DEVICE_ATTR_RO(recovery_state);
static DEVICE_ATTR_WO(recover);

static int mt6580_pwrap_link_check(struct mt6580_pwrap *wrp)
{
	unsigned int value = 0;
	unsigned long flags;
	int ret;

	/* Fixed, side-effect-free DEW signature, also used by the MT6580 BSP. */
	ret = regmap_read(wrp->regmap, MT6350_DEW_READ_TEST, &value);
	if (!ret && value != MT6350_READ_TEST_VALUE) {
		ret = -EIO;
		mt6580_pwrap_request_recovery(wrp);
	}
	spin_lock_irqsave(&wrp->stats_lock, flags);
	wrp->link.checks++;
	if (ret)
		wrp->link.check_errors++;
	wrp->link.test_value = value;
	wrp->link.test_error = ret;
	spin_unlock_irqrestore(&wrp->stats_lock, flags);
	return ret;
}

static int mt6580_pwrap_retention_snapshot(struct mt6580_pwrap *wrp, unsigned int *values)
{
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(retention_regs); i++) {
		ret = regmap_read(wrp->regmap, retention_regs[i], &values[i]);
		if (ret)
			return ret;
	}
	return 0;
}

static int mt6580_pwrap_update_checked(struct mt6580_pwrap *wrp, unsigned int reg,
				      unsigned int mask, unsigned int value)
{
	unsigned int after;
	int ret = regmap_update_bits(wrp->regmap, reg, mask, value);

	if (!ret)
		ret = regmap_read(wrp->regmap, reg, &after);
	if (!ret && ((after ^ value) & mask))
		ret = -EIO;
	return ret;
}

static int mt6580_pwrap_snapshot_vproc(struct mt6580_pwrap *wrp, unsigned int *values)
{
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(vproc_snapshot_regs); i++) {
		ret = regmap_read(wrp->regmap, vproc_snapshot_regs[i], &values[i]);
		if (ret)
			return ret;
	}
	return 0;
}

static int mt6580_pwrap_restore_bank(struct mt6580_pwrap *wrp)
{
	unsigned int value;
	int i, ret;

	if (!wrp->bank_active)
		return 0;
	/* Reassert the original clock policy before undoing bank ownership. */
	if (wrp->bank_mode == MT6580_VPROC_RETENTION_TEST) {
		ret = mt6580_pwrap_update_checked(wrp, PMIC_CLK_CTRL, PMIC_CLK_MASK,
						wrp->retention_before[0]);
		if (ret)
			return ret;
	}
	/* Return voltage ownership before CPUFreq can write its software bank.
	 * The software selector was left at the qualified 1.15 V suspend OPP.
	 */
	ret = regmap_update_bits(wrp->regmap, VPROC_CTRL, VPROC_HW_VOSEL,
				 wrp->vproc_before[0] & VPROC_HW_VOSEL);
	if (ret)
		return ret;
	ret = regmap_read_poll_timeout(wrp->regmap, VPROC_ACTUAL, value,
		(value & VPROC_SEL_MASK) == (wrp->vproc_before[4] & VPROC_SEL_MASK),
		10, 10000);
	if (ret)
		return ret;
	ret = regmap_update_bits(wrp->regmap, VPROC_ON, VPROC_SEL_MASK,
				 wrp->vproc_before[2]);
	if (ret)
		return ret;
	if (wrp->bank_mode == MT6580_VPROC_RETENTION_TEST) {
		for (i = ARRAY_SIZE(retention_regs) - 1; i > 0; i--) {
			ret = mt6580_pwrap_update_checked(wrp, retention_regs[i],
				retention_masks[i], wrp->retention_before[i]);
			if (ret)
				return ret;
		}
		ret = mt6580_pwrap_update_checked(wrp, VPROC_SLEEP, VPROC_SEL_MASK,
						wrp->vproc_before[3]);
		if (ret)
			return ret;
		ret = mt6580_pwrap_retention_snapshot(wrp, wrp->retention_restored);
		if (ret)
			return ret;
	}
	ret = mt6580_pwrap_snapshot_vproc(wrp, wrp->bank_restored);
	if (ret)
		return ret;
	if (((wrp->bank_restored[0] ^ wrp->vproc_before[0]) & VPROC_HW_VOSEL) ||
	    ((wrp->bank_restored[2] ^ wrp->vproc_before[2]) & VPROC_SEL_MASK) ||
	    ((wrp->bank_restored[3] ^ wrp->vproc_before[3]) & VPROC_SEL_MASK) ||
	    ((wrp->bank_restored[5] ^ wrp->vproc_before[5]) &
	     (VPROC_TRANS_MASK | VPROC_SLEEP_SEL)) ||
	    ((wrp->bank_restored[1] ^ wrp->vproc_before[1]) & VPROC_SEL_MASK) ||
	    ((wrp->bank_restored[4] ^ wrp->vproc_before[4]) & VPROC_SEL_MASK))
		return -EIO;
	wrp->bank_active = false;
	wrp->bank_restores++;
	return 0;
}

static int mt6580_pwrap_bank_snapshot(struct mt6580_pwrap *wrp, unsigned int *values)
{
	int ret = mt6580_pwrap_snapshot_vproc(wrp, values);
	unsigned int actual;

	if (ret)
		return ret;
	actual = values[4] & VPROC_SEL_MASK;
	/* This test never requests undervoltage: both possible banks are at
	 * the proven active floor or one 6.25 mV step above it.
	 */
	if (actual != VPROC_FLOOR_SEL && actual != VPROC_MARKER_SEL)
		return -EIO;
	if (actual == VPROC_MARKER_SEL)
		wrp->bank_markers++;
	return 0;
}

static int mt6580_pwrap_arm_retention(struct mt6580_pwrap *wrp)
{
	unsigned int actual;
	int ret;

	/* Exact MT6580 BSP fields: sleep reference 2, falling/rising soft
	 * change, digital sleep selector 0x18 and transition/VSLEEP enable.
	 * Software and ON selectors stay at 0x48. Only the final clock handoff
	 * allows hardware to observe SRCLKEN falling during PCM suspend.
	 */
	ret = mt6580_pwrap_update_checked(wrp, VPROC_SLP, VPROC_SLP_MASK, 2 << 4);
	if (!ret)
		ret = mt6580_pwrap_update_checked(wrp, VPROC_SLEW, VPROC_SLEW_MASK, 0x8097);
	if (!ret)
		ret = mt6580_pwrap_update_checked(wrp, VPROC_SLEEP,
						VPROC_SEL_MASK, VPROC_RETENTION_SEL);
	if (!ret)
		ret = mt6580_pwrap_update_checked(wrp, VPROC_CTRL, VPROC_HW_VOSEL, VPROC_HW_VOSEL);
	if (!ret)
		ret = mt6580_pwrap_update_checked(wrp, VPROC_TRANS, VPROC_TRANS_MASK, 0x113);
	if (!ret)
		ret = regmap_read(wrp->regmap, VPROC_ACTUAL, &actual);
	if (!ret && (actual & VPROC_SEL_MASK) != VPROC_FLOOR_SEL)
		ret = -EIO;
	if (!ret)
		ret = mt6580_pwrap_update_checked(wrp, PMIC_CLK_CTRL, PMIC_CLK_MASK, PMIC_CLK_MASK);
	if (!ret)
		ret = mt6580_pwrap_snapshot_vproc(wrp, wrp->bank_armed);
	if (!ret)
		ret = mt6580_pwrap_retention_snapshot(wrp, wrp->retention_armed);
	if (!ret && (wrp->bank_armed[4] & VPROC_SEL_MASK) != VPROC_FLOOR_SEL)
		ret = -EIO;
	return ret;
}

static int mt6580_pwrap_arm_bank(struct mt6580_pwrap *wrp, unsigned int mode)
{
	int ret;

	if (wrp->bank_active)
		return -EBUSY;
	wrp->bank_error = 0;
	wrp->bank_mode = mode;
	wrp->retention_resume_error = -ENODATA;
	memset(wrp->retention_before, 0, sizeof(wrp->retention_before));
	memset(wrp->retention_armed, 0, sizeof(wrp->retention_armed));
	memset(wrp->retention_resume, 0, sizeof(wrp->retention_resume));
	memset(wrp->retention_restored, 0, sizeof(wrp->retention_restored));
	memset(wrp->bank_armed, 0, sizeof(wrp->bank_armed));
	memset(wrp->bank_forced, 0, sizeof(wrp->bank_forced));
	memset(wrp->bank_released, 0, sizeof(wrp->bank_released));
	memset(wrp->bank_restored, 0, sizeof(wrp->bank_restored));
	if (wrp->vproc_before_error ||
	    (wrp->vproc_before[0] & VPROC_HW_VOSEL) ||
	    (wrp->vproc_before[5] & (GENMASK(5, 4) | VPROC_SLEEP_EN | VPROC_SLEEP_SEL)) ||
	    (wrp->vproc_before[1] & VPROC_SEL_MASK) != VPROC_FLOOR_SEL ||
	    (wrp->vproc_before[2] & VPROC_SEL_MASK) != VPROC_FLOOR_SEL ||
	    (wrp->vproc_before[3] & VPROC_SEL_MASK) != VPROC_FLOOR_SEL ||
	    (wrp->vproc_before[4] & VPROC_SEL_MASK) != VPROC_FLOOR_SEL)
		return -EBUSY;
	if (mode == MT6580_VPROC_RETENTION_TEST) {
		ret = mt6580_pwrap_retention_snapshot(wrp, wrp->retention_before);
		if (ret)
			return ret;
		/* Do not borrow a clock policy already owned by firmware. */
		if ((wrp->retention_before[0] & PMIC_CLK_MASK) != BIT(0))
			return -EBUSY;
	}
	/* Keep ownership across a partial setup so platform finish rolls it
	 * back before the software-bank regulator driver resumes.
	 */
	wrp->bank_active = true;
	if (mode == MT6580_VPROC_RETENTION_TEST) {
		ret = mt6580_pwrap_arm_retention(wrp);
		goto done;
	}
	ret = regmap_update_bits(wrp->regmap, VPROC_ON, VPROC_SEL_MASK, VPROC_MARKER_SEL);
	if (!ret)
		ret = regmap_update_bits(wrp->regmap, VPROC_CTRL, VPROC_HW_VOSEL, VPROC_HW_VOSEL);
	if (!ret) {
		usleep_range(100, 150);
		ret = mt6580_pwrap_bank_snapshot(wrp, wrp->bank_forced);
		if (!ret && (wrp->bank_forced[4] & VPROC_SEL_MASK) != VPROC_MARKER_SEL)
			ret = -EIO;
	}
	if (!ret)
		ret = regmap_update_bits(wrp->regmap, VPROC_ON, VPROC_SEL_MASK, VPROC_FLOOR_SEL);
	if (!ret) {
		usleep_range(100, 150);
		ret = mt6580_pwrap_bank_snapshot(wrp, wrp->bank_released);
		if (!ret && (wrp->bank_released[4] & VPROC_SEL_MASK) != VPROC_FLOOR_SEL)
			ret = -EIO;
	}
	if (!ret)
		ret = regmap_update_bits(wrp->regmap, VPROC_ON, VPROC_SEL_MASK, VPROC_MARKER_SEL);
	if (!ret) {
		usleep_range(100, 150);
		ret = mt6580_pwrap_bank_snapshot(wrp, wrp->bank_armed);
		if (!ret && (wrp->bank_armed[4] & VPROC_SEL_MASK) != VPROC_MARKER_SEL)
			ret = -EIO;
	}
done:
	wrp->bank_error = ret;
	if (!ret)
		wrp->bank_entries++;
	return ret;
}

static int mt6580_pwrap_restore_slots(struct mt6580_pwrap *wrp)
{
	int i, ret = 0;

	for (i = 0; i < ARRAY_SIZE(suspend_slot_regs); i++)
		writel(wrp->saved_slots[i], wrp->base + suspend_slot_regs[i]);
	for (i = 0; i < ARRAY_SIZE(suspend_slot_regs); i++)
		if (readl(wrp->base + suspend_slot_regs[i]) != wrp->saved_slots[i])
			ret = -EIO;
	if (!ret) {
		wrp->suspend_phase = false;
		wrp->restored++;
	}
	return ret;
}

/* Called in sleepable suspend preparation, before CPUs/IRQs are disabled.
 * Staging AUXADC commands does not execute them. The optional bank
 * transaction separately owns checked PMIC configuration until finish.
 * WACS2 voltage/RTC users do not use the autonomous AUXADC command slots.
 */
int mt6580_pwrap_prepare_suspend(struct device *dev, enum mt6580_vproc_sleep_mode mode)
{
	struct mt6580_pwrap *wrp = dev_get_drvdata(dev);
	unsigned int value;
	int i, ret;

	mutex_lock(&wrp->phase_lock);
	if (wrp->suspend_phase || wrp->bank_active || READ_ONCE(wrp->fault_pending) ||
	    READ_ONCE(wrp->failed)) {
		ret = -EBUSY;
		goto out;
	}
	ret = mt6580_pwrap_link_check(wrp);
	if (ret)
		goto out;
	/* CPUFreq has already selected its suspend OPP. Observe the actual
	 * selector before any optional bank transaction changes ownership.
	 */
	memset(wrp->vproc_before, 0, sizeof(wrp->vproc_before));
	memset(wrp->vproc_resume, 0, sizeof(wrp->vproc_resume));
	wrp->vproc_resume_error = -ENODATA;
	wrp->vproc_before_error = mt6580_pwrap_snapshot_vproc(wrp, wrp->vproc_before);
	ret = regmap_read(wrp->regmap, MT6350_STRUP_CON10, &value);
	if (ret)
		goto out;
	wrp->saved_auxadc = value;
	for (i = 0; i < ARRAY_SIZE(suspend_slot_regs); i++)
		wrp->saved_slots[i] = readl(wrp->base + suspend_slot_regs[i]);
	wrp->suspend_slots[0] = MT6350_STRUP_CON10;
	wrp->suspend_slots[1] = value & ~AUXADC_START;
	wrp->suspend_slots[2] = MT6350_STRUP_CON10;
	wrp->suspend_slots[3] = value | AUXADC_START;
	for (i = 0; i < ARRAY_SIZE(suspend_slot_regs); i++)
		writel(wrp->suspend_slots[i], wrp->base + suspend_slot_regs[i]);
	wrp->suspend_phase = true;
	for (i = 0; i < ARRAY_SIZE(suspend_slot_regs); i++) {
		if (readl(wrp->base + suspend_slot_regs[i]) != wrp->suspend_slots[i]) {
			mt6580_pwrap_restore_slots(wrp);
			ret = -EIO;
			goto out;
		}
	}
	wrp->prepared++;
	if (mode != MT6580_VPROC_RETAIN) {
		ret = mt6580_pwrap_arm_bank(wrp, mode == MT6580_VPROC_RETENTION_AUTO ?
					  MT6580_VPROC_RETENTION_TEST : mode);
		/* A firmware-owned or unsupported baseline can retain the previous
		 * voltage policy. Only skip before any bank write; real bus/setup
		 * or recovery errors still abort and unwind the transaction.
		 */
		if (mode == MT6580_VPROC_RETENTION_AUTO && ret == -EBUSY && !wrp->bank_active) {
			wrp->retention_skips++;
			ret = 0;
		}
	}
out:
	mutex_unlock(&wrp->phase_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6580_pwrap_prepare_suspend);

int mt6580_pwrap_finish_suspend(struct device *dev)
{
	struct mt6580_pwrap *wrp = dev_get_drvdata(dev);
	bool bank_active, check_link;
	int ret = 0, restore;

	mutex_lock(&wrp->phase_lock);
	bank_active = wrp->bank_active;
	check_link = wrp->suspend_phase;
	if (wrp->bank_active) {
		/* Sample hardware ownership before returning to the software bank. */
		wrp->vproc_resume_error = mt6580_pwrap_snapshot_vproc(wrp, wrp->vproc_resume);
		if (wrp->bank_mode == MT6580_VPROC_RETENTION_TEST)
			wrp->retention_resume_error =
				mt6580_pwrap_retention_snapshot(wrp, wrp->retention_resume);
		ret = mt6580_pwrap_restore_bank(wrp);
		if (ret)
			panic("MT6580: VPROC bank ownership did not recover (%d)", ret);
	}
	if (wrp->suspend_phase) {
		/* Platform finish still precedes CPUFreq governor/device resume. */
		if (!bank_active)
			wrp->vproc_resume_error =
				mt6580_pwrap_snapshot_vproc(wrp, wrp->vproc_resume);
		ret = regmap_read(wrp->regmap, MT6350_STRUP_CON10, &wrp->resume_auxadc);
		if (ret)
			goto restore_slots;
		/* PCM may have executed the AUXADC pulse sequence. Restore only
		 * START, retaining any unrelated PMIC state changed since entry.
		 */
		ret = regmap_update_bits(wrp->regmap, MT6350_STRUP_CON10,
					 AUXADC_START, wrp->saved_auxadc & AUXADC_START);
		if (ret)
			goto restore_slots;
		ret = regmap_read(wrp->regmap, MT6350_STRUP_CON10, &wrp->restored_auxadc);
restore_slots:
		/* PCM is stopped: release its slots even if the PMIC bus failed. */
		restore = mt6580_pwrap_restore_slots(wrp);
		if (!ret)
			ret = restore;
	}
	if (!ret && check_link)
		ret = mt6580_pwrap_link_check(wrp);
	mutex_unlock(&wrp->phase_lock);
	if (READ_ONCE(wrp->fault_pending))
		mt6580_pwrap_request_recovery(wrp);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6580_pwrap_finish_suspend);

static ssize_t spm_phase_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct mt6580_pwrap *wrp = dev_get_drvdata(dev);
	ssize_t len;

	mutex_lock(&wrp->phase_lock);
	len = sysfs_emit(buf, "active=%u prepared=%u restored=%u\n"
			 "saved=%08x/%08x/%08x/%08x\n"
			 "staged=%08x/%08x/%08x/%08x\n"
			 "auxadc=%04x/%04x/%04x (saved/resume/restored)\n",
			 wrp->suspend_phase, wrp->prepared, wrp->restored,
			 wrp->saved_slots[0], wrp->saved_slots[1],
			 wrp->saved_slots[2], wrp->saved_slots[3],
			 wrp->suspend_slots[0], wrp->suspend_slots[1],
			 wrp->suspend_slots[2], wrp->suspend_slots[3],
			 wrp->saved_auxadc, wrp->resume_auxadc, wrp->restored_auxadc);
	mutex_unlock(&wrp->phase_lock);
	return len;
}
static DEVICE_ATTR_RO(spm_phase);

static ssize_t sleep_voltage_show(struct device *dev, struct device_attribute *attr,
				 char *buf)
{
	struct mt6580_pwrap *wrp = dev_get_drvdata(dev);
	ssize_t len;

	mutex_lock(&wrp->phase_lock);
	len = sysfs_emit(buf, "errors=%d/%d (before/resume)\n"
			 "regs=0216/021e/0220/0222/0224/0230\n"
			 "before=%04x/%04x/%04x/%04x/%04x/%04x\n"
			 "resume=%04x/%04x/%04x/%04x/%04x/%04x\n",
			 wrp->vproc_before_error, wrp->vproc_resume_error,
			 wrp->vproc_before[0], wrp->vproc_before[1], wrp->vproc_before[2],
			 wrp->vproc_before[3], wrp->vproc_before[4], wrp->vproc_before[5],
			 wrp->vproc_resume[0], wrp->vproc_resume[1], wrp->vproc_resume[2],
			 wrp->vproc_resume[3], wrp->vproc_resume[4], wrp->vproc_resume[5]);
	len += sysfs_emit_at(buf, len,
		"bank active=%u entries=%u restores=%u markers=%u error=%d\n"
		"bank_armed=%04x/%04x/%04x/%04x/%04x/%04x\n"
		"bank_forced=%04x/%04x/%04x/%04x/%04x/%04x\n"
		"bank_released=%04x/%04x/%04x/%04x/%04x/%04x\n"
		"bank_restored=%04x/%04x/%04x/%04x/%04x/%04x\n",
		wrp->bank_active, wrp->bank_entries, wrp->bank_restores,
		wrp->bank_markers, wrp->bank_error,
		wrp->bank_armed[0], wrp->bank_armed[1], wrp->bank_armed[2],
		wrp->bank_armed[3], wrp->bank_armed[4], wrp->bank_armed[5],
		wrp->bank_forced[0], wrp->bank_forced[1], wrp->bank_forced[2],
		wrp->bank_forced[3], wrp->bank_forced[4], wrp->bank_forced[5],
		wrp->bank_released[0], wrp->bank_released[1], wrp->bank_released[2],
		wrp->bank_released[3], wrp->bank_released[4], wrp->bank_released[5],
		wrp->bank_restored[0], wrp->bank_restored[1], wrp->bank_restored[2],
		wrp->bank_restored[3], wrp->bank_restored[4], wrp->bank_restored[5]);
	len += sysfs_emit_at(buf, len,
		"bank_mode=%u skips=%u resume_error=%d retention_regs=0120/0212/021c/0230\n"
		"retention_before=%04x/%04x/%04x/%04x\n"
		"retention_armed=%04x/%04x/%04x/%04x\n"
		"retention_resume=%04x/%04x/%04x/%04x\n"
		"retention_restored=%04x/%04x/%04x/%04x\n",
		wrp->bank_mode, wrp->retention_skips, wrp->retention_resume_error,
		wrp->retention_before[0], wrp->retention_before[1],
		wrp->retention_before[2], wrp->retention_before[3],
		wrp->retention_armed[0], wrp->retention_armed[1],
		wrp->retention_armed[2], wrp->retention_armed[3],
		wrp->retention_resume[0], wrp->retention_resume[1],
		wrp->retention_resume[2], wrp->retention_resume[3],
		wrp->retention_restored[0], wrp->retention_restored[1],
		wrp->retention_restored[2], wrp->retention_restored[3]);
	mutex_unlock(&wrp->phase_lock);
	return len;
}
static DEVICE_ATTR_RO(sleep_voltage);

static ssize_t link_state_show(struct device *dev, struct device_attribute *attr,
			       char *buf)
{
	struct mt6580_pwrap *wrp = dev_get_drvdata(dev);
	struct mt6580_pwrap_link_stats link;
	unsigned long flags;
	u32 status;
	ssize_t len;

	mutex_lock(&wrp->phase_lock);
	mt6580_pwrap_link_check(wrp);
	spin_lock_irqsave(&wrp->stats_lock, flags);
	link = wrp->link;
	spin_unlock_irqrestore(&wrp->stats_lock, flags);
	status = readl(wrp->base + WACS2_RDATA);
	len = sysfs_emit(buf,
		"init_done=%u wacs2=%08x\n"
		"source=%s firmware_at_probe=%u scan_mask=%06x sample=%u stage=%s\n"
		"read_test=%04x expected=%04x error=%d checks=%u failures=%u\n"
		"reads=%u writes=%u errors=%u timeouts=%u init_lost=%u stale_valid_clears=%u\n"
		"last_error=%d reg=%04x write=%u stage=%u status=%08x\n",
		!!(status & WACS_INIT_DONE), status,
		wrp->cold_initialized ? "kernel" : "firmware", wrp->firmware_initialized,
		wrp->si_pass_mask, wrp->si_point, wrp->init_stage,
		link.test_value, MT6350_READ_TEST_VALUE, link.test_error,
		link.checks, link.check_errors, link.reads, link.writes,
		link.errors, link.timeouts, link.init_lost, link.stale_valid_clears,
		link.last_error, link.last_reg, link.last_write,
		link.last_stage, link.last_status);
	mutex_unlock(&wrp->phase_lock);
	return len;
}
static DEVICE_ATTR_RO(link_state);

static struct attribute *mt6580_pwrap_attrs[] = {
	&dev_attr_spm_phase.attr,
	&dev_attr_sleep_voltage.attr,
	&dev_attr_link_state.attr,
	&dev_attr_recovery_state.attr,
	&dev_attr_recover.attr,
	NULL,
};
ATTRIBUTE_GROUPS(mt6580_pwrap);

static u32 mt6580_pwrap_poll_status(struct mt6580_pwrap *wrp, bool clear_stale,
				  unsigned int *clears)
{
	u32 status = readl(wrp->base + WACS2_RDATA);

	/* A previous timed-out read may complete while we wait for IDLE.
	 * Only discard it before issuing the next command, never while waiting
	 * for the current command's result. Do not touch an uninitialized bus.
	 */
	if (clear_stale && ((status & WACS_INIT_DONE) || wrp->initializing) &&
	    FIELD_GET(WACS_FSM, status) == WACS_VALID) {
		writel(1, wrp->base + WACS2_VLDCLR);
		(*clears)++;
	}
	return status;
}

static int mt6580_pwrap_wait(struct mt6580_pwrap *wrp, unsigned int fsm,
			    bool clear_stale, u32 *status, unsigned int *clears)
{
	int ret;

	/* Keep polling atomic: final PMIC power-off has interrupts disabled. */
	ret = read_poll_timeout_atomic(mt6580_pwrap_poll_status, *status,
			(!wrp->initializing && !(*status & WACS_INIT_DONE)) ||
			FIELD_GET(WACS_FSM, *status) == fsm,
			10, 10000, false, wrp, clear_stale, clears);
	if (!wrp->initializing && !(*status & WACS_INIT_DONE))
		return -ENODEV;
	return ret;
}

/* regmap serializes all users, including RTC reboot and regulator accesses.
 * A failed transaction is never retried: writes may already have committed,
 * and reads such as interrupt status may have side effects.
 */
static int mt6580_pwrap_xfer(struct mt6580_pwrap *wrp, unsigned int reg,
			   unsigned int *value, bool write)
{
	enum mt6580_pwrap_wait_stage stage = PWRAP_WAIT_IDLE;
	unsigned int clears = 0;
	unsigned long flags;
	u32 status;
	int ret;

	ret = mt6580_pwrap_wait(wrp, WACS_IDLE, true, &status, &clears);
	if (ret)
		goto out;

	writel((write ? BIT(31) : 0) | ((reg >> 1) << 16) |
	       (write ? *value : 0), wrp->base + WACS2_CMD);
	stage = write ? PWRAP_WAIT_WRITE : PWRAP_WAIT_READ;
	ret = mt6580_pwrap_wait(wrp, write ? WACS_IDLE : WACS_VALID,
				false, &status, &clears);
	if (!ret && !write) {
		*value = status & 0xffff;
		writel(1, wrp->base + WACS2_VLDCLR);
	}
out:
	/* This separate lock only protects diagnostics, never encloses a bus
	 * access, and can also be taken by the read-only sysfs snapshot.
	 */
	spin_lock_irqsave(&wrp->stats_lock, flags);
	if (write)
		wrp->link.writes++;
	else
		wrp->link.reads++;
	wrp->link.stale_valid_clears += clears;
	if (ret) {
		wrp->link.errors++;
		if (ret == -ETIMEDOUT)
			wrp->link.timeouts++;
		if (ret == -ENODEV)
			wrp->link.init_lost++;
		wrp->link.last_error = ret;
		wrp->link.last_reg = reg;
		wrp->link.last_write = write;
		wrp->link.last_stage = stage;
		wrp->link.last_status = status;
	}
	spin_unlock_irqrestore(&wrp->stats_lock, flags);
	return ret;
}

/* Public map holds bus_lock. Freeze new traffic while rebuilding, but do
 * not prevent a healthy WDT-only suspend transaction from restoring VPROC.
 */
static int mt6580_pwrap_public_xfer(struct mt6580_pwrap *wrp, unsigned int reg,
				    unsigned int *value, bool write)
{
	u32 raw;
	int ret;

	if (wrp->recovering || wrp->failed ||
	    (READ_ONCE(wrp->fault_pending) && !READ_ONCE(wrp->suspend_phase))) {
		wrp->blocked++;
		return wrp->failed ? -EIO : -EAGAIN;
	}
	raw = readl(wrp->base + PWRAP_INT_RAW) | READ_ONCE(wrp->fault_flags);
	if (raw & (PWRAP_ERROR_MASK & ~BIT(0))) {
		mt6580_pwrap_request_recovery(wrp);
		return -EIO;
	}
	ret = mt6580_pwrap_xfer(wrp, reg, value, write);
	raw = readl(wrp->base + PWRAP_INT_RAW);
	if (ret || (raw & PWRAP_ERROR_MASK))
		mt6580_pwrap_request_recovery(wrp);
	if (!ret && (raw & (PWRAP_ERROR_MASK & ~BIT(0))))
		ret = -EIO;
	return ret;
}

static int mt6580_pwrap_read(void *context, unsigned int reg, unsigned int *value)
{
	return mt6580_pwrap_public_xfer(context, reg, value, false);
}

static int mt6580_pwrap_write(void *context, unsigned int reg, unsigned int value)
{
	return mt6580_pwrap_public_xfer(context, reg, &value, true);
}

static void mt6580_pwrap_bus_lock(void *context)
{
	struct mt6580_pwrap *wrp = context;
	unsigned long flags;

	spin_lock_irqsave(&wrp->bus_lock, flags);
	wrp->bus_flags = flags;
}

static void mt6580_pwrap_bus_unlock(void *context)
{
	struct mt6580_pwrap *wrp = context;

	spin_unlock_irqrestore(&wrp->bus_lock, wrp->bus_flags);
}

/* Only probe or recovery under phase_lock, with public clients excluded. */
static int mt6580_pwrap_init_read(void *context, unsigned int reg, unsigned int *value)
{
	return mt6580_pwrap_xfer(context, reg, value, false);
}

static int mt6580_pwrap_init_write(void *context, unsigned int reg, unsigned int value)
{
	return mt6580_pwrap_xfer(context, reg, &value, true);
}

static const struct regmap_config mt6580_pwrap_regmap = {
	/* Final PMIC power-off runs with interrupts disabled. */
	.fast_io = true,
	.reg_bits = 16,
	.val_bits = 16,
	.reg_stride = 2,
	.max_register = 0xfffe,
	.reg_read = mt6580_pwrap_read,
	.reg_write = mt6580_pwrap_write,
	.lock = mt6580_pwrap_bus_lock,
	.unlock = mt6580_pwrap_bus_unlock,
};

static int mt6580_pwrap_sync_idle(struct mt6580_pwrap *wrp)
{
	u32 status;

	return readl_poll_timeout_atomic(wrp->base + WACS2_RDATA, status,
		FIELD_GET(WACS_FSM, status) == WACS_IDLE && (status & WACS_SYNC_IDLE),
		10, 10000);
}

/* A nonempty, single continuous window is required before __ffs/fls or
 * selecting a center. The BSP's empty-window arithmetic was undefined.
 */
static int mt6580_pwrap_select_sample(u32 mask)
{
	unsigned int first, last;

	if (!mask || (mask & ~GENMASK(23, 0)))
		return -EINVAL;
	first = __ffs(mask);
	last = fls(mask) - 1;
	if (mask != GENMASK(last, first))
		return -EINVAL;
	return (first + last) / 2;
}

static int mt6580_pwrap_signature(struct mt6580_pwrap *wrp)
{
	unsigned int value;
	int ret = mt6580_pwrap_init_read(wrp, MT6350_DEW_READ_TEST, &value);

	return ret ?: (value == MT6350_READ_TEST_VALUE ? 0 : -EIO);
}

static int mt6580_pwrap_calibrate(struct mt6580_pwrap *wrp)
{
	unsigned int i, value;
	int ret, point;

	wrp->si_pass_mask = 0;
	for (i = 0; i < 24; i++) {
		writel(i >> 2, wrp->base + PWRAP_SI_CK_CON);
		writel(3 - (i & 3), wrp->base + PWRAP_SIDLY);
		ret = mt6580_pwrap_init_read(wrp, MT6350_DEW_READ_TEST, &value);
		/* A completed but incorrect read is a rejected sample. A bus
		 * timeout is different: do not change timing under an in-flight
		 * transfer or silently count its data as a later sample.
		 */
		if (ret)
			return ret;
		if (value == MT6350_READ_TEST_VALUE)
			wrp->si_pass_mask |= BIT(i);
	}
	point = mt6580_pwrap_select_sample(wrp->si_pass_mask);
	if (point < 0)
		return point;
	wrp->si_point = point;
	writel(point >> 2, wrp->base + PWRAP_SI_CK_CON);
	writel(3 - (point & 3), wrp->base + PWRAP_SIDLY);
	return mt6580_pwrap_signature(wrp);
}

static int mt6580_pwrap_reg_clock(struct mt6580_pwrap *wrp)
{
	unsigned int value;
	int ret;

	ret = mt6580_pwrap_init_write(wrp, 0x12a, 3); /* TOP_CKCON1_CLR */
	if (ret)
		return ret;
	ret = mt6580_pwrap_init_read(wrp, 0x126, &value);
	if (ret || (value & 3))
		return ret ?: -EIO;
	ret = mt6580_pwrap_init_write(wrp, 0x1a4, 8); /* DEW_RDDMY_NO */
	if (ret)
		return ret;
	writel(8, wrp->base + PWRAP_RDDMY);
	writel(0, wrp->base + PWRAP_CSHEXT_READ);
	writel(7, wrp->base + PWRAP_CSHEXT_WRITE);
	writel(0, wrp->base + PWRAP_CSLEXT_START);
	writel(0, wrp->base + PWRAP_CSLEXT_END);
	return mt6580_pwrap_signature(wrp);
}

static int mt6580_pwrap_dio(struct mt6580_pwrap *wrp)
{
	int ret = mt6580_pwrap_init_write(wrp, 0x18a, 1);

	if (!ret)
		ret = mt6580_pwrap_sync_idle(wrp);
	if (ret)
		return ret;
	writel(1, wrp->base + PWRAP_DIO_EN);
	return mt6580_pwrap_signature(wrp);
}

static int mt6580_pwrap_cipher(struct mt6580_pwrap *wrp)
{
	static const struct reg_sequence setup[] = {
		{ 0x1a2, 1 }, { 0x1a2, 0 }, /* SWRST */
		{ 0x198, 1 }, { 0x19a, 2 }, { 0x19c, 1 }, /* key, IV, EN */
	};
	unsigned int value, i;
	u32 ready;
	int ret;

	writel(1, wrp->base + PWRAP_CIPHER_SWRST);
	writel(0, wrp->base + PWRAP_CIPHER_SWRST);
	writel(1, wrp->base + PWRAP_CIPHER_KEY);
	writel(2, wrp->base + PWRAP_CIPHER_IV);
	writel(1, wrp->base + PWRAP_CIPHER_EN);
	for (i = 0; i < ARRAY_SIZE(setup); i++) {
		ret = mt6580_pwrap_init_write(wrp, setup[i].reg, setup[i].def);
		if (ret)
			return ret;
	}
	ret = readl_poll_timeout_atomic(wrp->base + PWRAP_CIPHER_RDY, ready,
					ready == 1, 10, 10000);
	if (ret)
		return ret;
	/* The BSP logs timeout but loops forever here. Bound both attempts
	 * and each underlying WACS2 transaction; propagate bus errors.
	 */
	for (i = 0; i < 1000; i++) {
		ret = mt6580_pwrap_init_read(wrp, 0x19e, &value);
		if (ret)
			return ret;
		if (value == 1)
			break;
		udelay(10);
	}
	if (i == 1000)
		return -ETIMEDOUT;
	ret = mt6580_pwrap_init_write(wrp, 0x1a0, 1);
	if (!ret)
		ret = mt6580_pwrap_sync_idle(wrp);
	if (ret)
		return ret;
	writel(1, wrp->base + PWRAP_CIPHER_MODE);
	return mt6580_pwrap_signature(wrp);
}

static void mt6580_pwrap_quiesce(struct mt6580_pwrap *wrp)
{
	/* A partially changed two-ended protocol cannot be rolled back by
	 * restoring AP registers. Publish no INIT_DONE or children on failure.
	 * No blind PMIC writes or command replay is attempted.
	 */
	writel(0, wrp->base + PWRAP_INIT_DONE0);
	writel(0, wrp->base + PWRAP_INIT_DONE1);
	writel(0, wrp->base + PWRAP_INIT_DONE2);
	writel(0, wrp->base + PWRAP_INT_EN);
	writel(0, wrp->base + PWRAP_STAUPD_GRPEN);
	writel(0, wrp->base + PWRAP_TIMER_EN);
	writel(0, wrp->base + PWRAP_WDT_SRC_EN);
	writel(0, wrp->base + PWRAP_ARB_EN);
	writel(0, wrp->base + PWRAP_WACS0_EN);
	writel(0, wrp->base + PWRAP_WACS1_EN);
	writel(0, wrp->base + PWRAP_WACS2_EN);
	writel(0, wrp->base + PWRAP_MAN_EN);
	writel(0, wrp->base + PWRAP_MUX_SEL);
	writel(0, wrp->base + PWRAP_WRAP_EN);
	readl(wrp->base + PWRAP_WRAP_EN);
}

static int mt6580_pwrap_cold_init(struct mt6580_pwrap *wrp, struct device *top,
				 struct reset_control *reset, struct clk *spi, struct clk *ref)
{
	static const u16 manual[] = { 0x2100, 0x2800, 0x2000, 0x2800, 0x2800, 0x2800, 0x2800 };
	unsigned int value, i;
	struct clk *source = clk_get_parent(spi), *osc = clk_get_parent(ref);
	u32 enabled, status;
	int ret, restore;

	if (!source || !osc)
		return -ENODEV;
	wrp->init_stage = "clock-reset";
	ret = mt6580_pwrap_clocks_gate(top, &enabled);
	if (ret)
		return ret;
	readl(wrp->base + PWRAP_SWRST); /* BSP delay after module clock gating */
	writel(1, wrp->base + PWRAP_SWRST);
	ret = reset_control_reset(reset);
	writel(0, wrp->base + PWRAP_SWRST);
	restore = mt6580_pwrap_clocks_restore(top, enabled);
	if (ret || restore) {
		ret = ret ?: restore;
		goto fail;
	}
	ret = clk_set_parent(source, osc);
	if (ret || clk_get_rate(spi) != 26000000) {
		ret = ret ?: -EINVAL;
		goto fail;
	}

	wrp->initializing = true;
	wrp->init_stage = "spi-slave-reset";
	writel(1, wrp->base + PWRAP_DCM_EN);
	writel(0, wrp->base + PWRAP_DCM_DBC);
	writel(0, wrp->base + PWRAP_ARB_EN);
	writel(0, wrp->base + PWRAP_WRAP_EN);
	writel(1, wrp->base + PWRAP_MUX_SEL);
	writel(1, wrp->base + PWRAP_MAN_EN);
	writel(0, wrp->base + PWRAP_DIO_EN);
	for (i = 0; i < ARRAY_SIZE(manual); i++)
		writel(manual[i], wrp->base + PWRAP_MAN_CMD);
	ret = readl_poll_timeout_atomic(wrp->base + WACS2_RDATA, status,
					status & WACS_SYNC_IDLE, 10, 10000);
	writel(0, wrp->base + PWRAP_MAN_EN);
	writel(0, wrp->base + PWRAP_MUX_SEL);
	if (ret)
		goto fail;
	writel(1, wrp->base + PWRAP_WRAP_EN);
	writel(PWRAP_WACS2_ARB, wrp->base + PWRAP_ARB_EN);
	writel(1, wrp->base + PWRAP_WACS2_EN);
	writel(15, wrp->base + PWRAP_RDDMY);
	wrp->init_stage = "sampling";
	ret = mt6580_pwrap_calibrate(wrp);
	if (ret)
		goto fail;
	wrp->init_stage = "register-clock";
	ret = mt6580_pwrap_reg_clock(wrp);
	if (ret)
		goto fail;
	wrp->init_stage = "dio";
	ret = mt6580_pwrap_dio(wrp);
	if (ret)
		goto fail;
	wrp->init_stage = "cipher";
	ret = mt6580_pwrap_cipher(wrp);
	if (ret)
		goto fail;
	wrp->init_stage = "write-test";
	ret = mt6580_pwrap_init_write(wrp, 0x18e, 0xa55a);
	if (!ret)
		ret = mt6580_pwrap_init_read(wrp, 0x18e, &value);
	if (!ret && value != 0xa55a)
		ret = -EIO;
	if (ret)
		goto fail;
	wrp->init_stage = "crc";
	ret = mt6580_pwrap_init_write(wrp, 0x192, 1);
	if (ret)
		goto fail;
	writel(1, wrp->base + PWRAP_CRC_EN);
	writel(0, wrp->base + PWRAP_SIG_MODE);
	writel(0x194, wrp->base + PWRAP_SIG_ADR);
	ret = mt6580_pwrap_signature(wrp);
	if (ret)
		goto fail;
	wrp->init_stage = "status-updates";
	/* An online rebuild must keep autonomous masters stopped until their
	 * saved slots and status/ADC configuration have been restored. WACS2
	 * alone remains available for private verification.
	 */
	if (!wrp->recovering) {
		writel(0x1ff, wrp->base + PWRAP_ARB_EN);
		writel(1, wrp->base + PWRAP_WACS0_EN);
		writel(1, wrp->base + PWRAP_WACS1_EN);
	}
	writel(5, wrp->base + PWRAP_STAUPD_PRD);
	writel(wrp->recovering ? 0 : 0xff, wrp->base + PWRAP_STAUPD_GRPEN);
	writel(0xf, wrp->base + PWRAP_WDT_UNIT);
	writel(0xffffffff, wrp->base + PWRAP_WDT_SRC_EN);
	writel(1, wrp->base + PWRAP_TIMER_EN);
	/* Probe has not registered the IRQ yet; recovery re-enables it only
	 * after restoring and checking the saved autonomous configuration.
	 */
	writel(0, wrp->base + PWRAP_INT_EN);
	writel(0xffffffff, wrp->base + PWRAP_INT_CLR);
	writel(0x76c, wrp->base + PWRAP_ADC_CMD_ADR);
	writel(0x8000, wrp->base + PWRAP_ADC_CMD);
	writel(0x72c, wrp->base + PWRAP_ADC_RDY_ADR);
	writel(0x72e, wrp->base + PWRAP_ADC_DATA1);
	writel(0x730, wrp->base + PWRAP_ADC_DATA2);
	writel(1, wrp->base + PWRAP_INIT_DONE2);
	writel(1, wrp->base + PWRAP_INIT_DONE0);
	writel(1, wrp->base + PWRAP_INIT_DONE1);
	wrp->initializing = false;
	wrp->init_stage = "final-check";
	ret = mt6580_pwrap_signature(wrp);
	if (ret)
		goto fail;
	wrp->cold_initialized = true;
	wrp->init_stage = "complete";
	return 0;
fail:
	mt6580_pwrap_quiesce(wrp);
	wrp->initializing = false;
	return ret;
}

static void mt6580_pwrap_request_recovery(struct mt6580_pwrap *wrp)
{
	unsigned long flags;
	bool first;

	if (!READ_ONCE(wrp->online) || READ_ONCE(wrp->stopping) ||
	    READ_ONCE(wrp->recovering) || READ_ONCE(wrp->failed))
		return;
	spin_lock_irqsave(&wrp->irq_lock, flags);
	if (READ_ONCE(wrp->stopping) || READ_ONCE(wrp->recovering) || READ_ONCE(wrp->failed)) {
		spin_unlock_irqrestore(&wrp->irq_lock, flags);
		return;
	}
	first = !wrp->fault_pending;
	wrp->fault_pending = true;
	wrp->fault_flags |= readl(wrp->base + PWRAP_INT_RAW) & PWRAP_ERROR_MASK;
	spin_unlock_irqrestore(&wrp->irq_lock, flags);
	if (first)
		__pm_wakeup_event(wrp->recovery_ws, 1000);
	mod_delayed_work(system_freezable_wq, &wrp->recovery_work, 0);
}

static irqreturn_t mt6580_pwrap_irq(int irq, void *data)
{
	struct mt6580_pwrap *wrp = data;
	unsigned long flags;
	u32 status;

	spin_lock_irqsave(&wrp->irq_lock, flags);
	status = readl(wrp->base + PWRAP_INT_FLG);
	if (!status) {
		spin_unlock_irqrestore(&wrp->irq_lock, flags);
		return IRQ_NONE;
	}
	wrp->irq_count++;
	wrp->irq_last = status;
	wrp->irq_raw = readl(wrp->base + PWRAP_INT_RAW);
	wrp->irq_wdt = readl(wrp->base + PWRAP_WDT_FLG);
	wrp->irq_signature = readl(wrp->base + PWRAP_SIG_ERRVAL);
	wrp->irq_wacs = readl(wrp->base + WACS2_RDATA);
	wrp->fault_flags |= (status | wrp->irq_raw) & PWRAP_ERROR_MASK;
	/* No WACS access or link reset in hard IRQ context. Mask the source
	 * until its bounded worker has checked/repaired it, avoiding a storm.
	 */
	writel(0, wrp->base + PWRAP_INT_EN);
	writel(status, wrp->base + PWRAP_INT_CLR);
	spin_unlock_irqrestore(&wrp->irq_lock, flags);
	mt6580_pwrap_request_recovery(wrp);
	return IRQ_HANDLED;
}

static int mt6580_pwrap_private_vproc(struct mt6580_pwrap *wrp, u32 values[6])
{
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(vproc_snapshot_regs); i++) {
		ret = mt6580_pwrap_init_read(wrp, vproc_snapshot_regs[i], &values[i]);
		if (ret)
			return ret;
	}
	return 0;
}

static void mt6580_pwrap_recovery_work(struct work_struct *work)
{
	struct mt6580_pwrap *wrp = container_of(to_delayed_work(work),
					     struct mt6580_pwrap, recovery_work);
	u32 saved[ARRAY_SIZE(recovery_regs)], causes, status;
	unsigned int clears = 0, i;
	unsigned long flags, irqflags;
	bool rebuild, forced;
	int ret, before = -ENODATA;

	mutex_lock(&wrp->phase_lock);
	if (wrp->stopping || !wrp->online)
		goto unlock;
	if (system_state < SYSTEM_RUNNING) {
		mod_delayed_work(system_freezable_wq, &wrp->recovery_work, HZ / 4);
		goto unlock;
	}
	if (system_state != SYSTEM_RUNNING)
		goto unlock;
	if (wrp->suspend_phase || wrp->bank_active) {
		wrp->recovery_deferred++;
		goto unlock; /* platform finish will queue the pending incident */
	}
	__pm_stay_awake(wrp->recovery_ws);
	/* Wait out the last complete regmap operation, including update_bits.
	 * Subsequent public callbacks fail transiently; the worker is the
	 * only serial client until publication below. Do not hold IRQs off over
	 * calibration, cipher setup, CCF or reset-controller operations.
	 */
	spin_lock_irqsave(&wrp->bus_lock, flags);
	wrp->recovering = true;
	spin_lock_irqsave(&wrp->irq_lock, irqflags);
	causes = wrp->fault_flags | (readl(wrp->base + PWRAP_INT_RAW) & PWRAP_ERROR_MASK);
	writel(0, wrp->base + PWRAP_INT_EN);
	spin_unlock_irqrestore(&wrp->irq_lock, irqflags);
	spin_unlock_irqrestore(&wrp->bus_lock, flags);
	if (wrp->irq > 0)
		synchronize_irq(wrp->irq);
	wrp->recovery_attempts++;
	if (time_before(jiffies, wrp->last_recovery + HZ))
		wrp->recovery_burst++;
	else
		wrp->recovery_burst = 1;
	wrp->last_recovery = jiffies;
	forced = wrp->force_rebuild;
	wrp->force_rebuild = false;
	ret = mt6580_pwrap_wait(wrp, WACS_IDLE, true, &status, &clears);
	if (!ret)
		ret = mt6580_pwrap_signature(wrp);
	/* WDT-only starvation or a delayed VALID may have recovered already.
	 * A non-WDT integrity/error flag needs a new two-ended protocol even
	 * if the fixed read signature happens to be correct now.
	 */
	/* Repeated WDT-only incidents must not create an unbounded soft
	 * recovery/IRQ loop either. Escalate a burst, then the full-rebuild
	 * rate limit below latches a persistent hardware fault.
	 */
	rebuild = forced || wrp->failed || ret || (causes & ~BIT(0)) ||
		wrp->recovery_burst >= 4;
	if (!rebuild) {
		wrp->recovery_soft++;
		goto publish;
	}
	if (!wrp->reset || !wrp->top || !wrp->spi || !wrp->ref) {
		ret = -EOPNOTSUPP;
		goto publish;
	}
	/* One automatic rebuild per minute, no reset/retry loop on a broken
	 * physical bus. An explicit administrator request can retry later.
	 */
	if (!forced && wrp->recovery_rebuilds &&
	    time_before(jiffies, wrp->last_rebuild + 60 * HZ)) {
		ret = -EAGAIN;
		goto publish;
	}
	/* A failed reset quiesces AP registers. Retain the last pre-reset
	 * configuration for an explicit later retry, not that disabled state.
	 */
	if (!wrp->failed) {
		for (i = 0; i < ARRAY_SIZE(recovery_regs); i++)
			wrp->recovery_config[i] = readl(wrp->base + recovery_regs[i]);
		wrp->recovery_config_valid = true;
	}
	if (!wrp->recovery_config_valid) {
		ret = -ENODATA;
		goto publish;
	}
	memcpy(saved, wrp->recovery_config, sizeof(saved));
	/* A regulator transaction may have completed its SPI write just
	 * before bus exclusion, while the analog selector is still settling.
	 * Wait beyond the BSP's maximum 127-step down-ramp (259 us) before
	 * comparing actual voltage across a reset that must not change rails.
	 */
	usleep_range(300, 500);
	before = mt6580_pwrap_private_vproc(wrp, wrp->recovery_before);
	wrp->recovery_snapshot_error = before;
	/* Stop new autonomous status requests, then drain the shared wrapper.
	 * A broken link may never drain: keep the bounded error as evidence
	 * and reset with all software clients and SPM commands excluded.
	 */
	writel(0, wrp->base + PWRAP_STAUPD_GRPEN);
	writel(PWRAP_WACS2_ARB, wrp->base + PWRAP_ARB_EN);
	wrp->recovery_drain_error = readl_poll_timeout_atomic(
		wrp->base + PWRAP_STAUPD_STA, status,
		!(status & GENMASK(8, 6)), 10, 10000);
	if (!wrp->recovery_drain_error)
		wrp->recovery_drain_error = readl_poll_timeout_atomic(
			wrp->base + PWRAP_WRAP_STA, status,
			!(status & (GENMASK(14, 11) | GENMASK(6, 5) | GENMASK(2, 0))),
			10, 10000);
	wrp->last_rebuild = jiffies;
	wrp->recovery_rebuilds++;
	ret = mt6580_pwrap_cold_init(wrp, wrp->top, wrp->reset, wrp->spi, wrp->ref);
	if (ret)
		goto publish;
	/* Restore slots/ADC routing/arbitration, not old serial/cipher state.
	 * Background masters are enabled only after their data/configuration.
	 */
	writel(0, wrp->base + PWRAP_ARB_EN);
	writel(0, wrp->base + PWRAP_STAUPD_GRPEN);
	for (i = 0; i < ARRAY_SIZE(recovery_regs); i++) {
		if (recovery_regs[i] != PWRAP_ARB_EN && recovery_regs[i] != PWRAP_STAUPD_GRPEN)
			writel(saved[i], wrp->base + recovery_regs[i]);
	}
	for (i = 0; i < ARRAY_SIZE(recovery_regs); i++) {
		if (recovery_regs[i] == PWRAP_STAUPD_GRPEN || recovery_regs[i] == PWRAP_ARB_EN)
			writel(saved[i], wrp->base + recovery_regs[i]);
	}
	for (i = 0; i < ARRAY_SIZE(recovery_regs); i++)
		if (readl(wrp->base + recovery_regs[i]) != saved[i]) {
			ret = -EIO;
			break;
		}
	if (!ret)
		ret = mt6580_pwrap_private_vproc(wrp, wrp->recovery_after);
	if (!ret && !before && memcmp(wrp->recovery_before, wrp->recovery_after,
				      sizeof(wrp->recovery_before)))
		ret = -EIO;
	if (!ret)
		ret = mt6580_pwrap_signature(wrp);
publish:
	wrp->recovery_error = ret;
	if (ret)
		wrp->recovery_failures++;
	/* Publish under the same lock used by all public-map callbacks. A
	 * failed reconstruction stays fail-closed; it never replays the failed
	 * regulator/IRQ/RTC operation or fabricates that operation's result.
	 */
	spin_lock_irqsave(&wrp->bus_lock, flags);
	spin_lock_irqsave(&wrp->irq_lock, irqflags);
	if (!ret) {
		writel(causes, wrp->base + PWRAP_INT_CLR);
		wrp->fault_flags = 0;
		wrp->fault_pending = false;
		writel(wrp->irq > 0 ? PWRAP_IRQ_MASK : 0, wrp->base + PWRAP_INT_EN);
	}
	wrp->failed = !!ret;
	wrp->recovering = false;
	spin_unlock_irqrestore(&wrp->irq_lock, irqflags);
	spin_unlock_irqrestore(&wrp->bus_lock, flags);
	__pm_relax(wrp->recovery_ws);
	if (ret)
		dev_err(wrp->dev, "PWRAP recovery failed: %d (flags=%08x)\n", ret, causes);
	else
		dev_info(wrp->dev, "PWRAP link recovered (%s, flags=%08x)\n",
			 rebuild ? "rebuilt" : "resynchronized", causes);
unlock:
	mutex_unlock(&wrp->phase_lock);
}

static ssize_t recovery_state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mt6580_pwrap *wrp = dev_get_drvdata(dev);
	unsigned long flags;
	ssize_t len;

	mutex_lock(&wrp->phase_lock);
	spin_lock_irqsave(&wrp->irq_lock, flags);
	len = sysfs_emit(buf,
		"irq=%d count=%u last=%08x raw=%08x wdt=%08x signature=%08x wacs=%08x\n"
		"pending=%u recovering=%u failed=%u attempts=%u soft=%u rebuilds=%u failures=%u deferred=%u blocked=%u error=%d drain_error=%d\n"
		"vproc_snapshot_error=%d before=%04x/%04x/%04x/%04x/%04x/%04x after=%04x/%04x/%04x/%04x/%04x/%04x\n",
		wrp->irq, wrp->irq_count, wrp->irq_last, wrp->irq_raw, wrp->irq_wdt,
		wrp->irq_signature, wrp->irq_wacs, wrp->fault_pending,
		wrp->recovering, wrp->failed, wrp->recovery_attempts, wrp->recovery_soft,
		wrp->recovery_rebuilds, wrp->recovery_failures, wrp->recovery_deferred,
		READ_ONCE(wrp->blocked), wrp->recovery_error, wrp->recovery_drain_error,
		wrp->recovery_snapshot_error,
		wrp->recovery_before[0], wrp->recovery_before[1], wrp->recovery_before[2],
		wrp->recovery_before[3], wrp->recovery_before[4], wrp->recovery_before[5],
		wrp->recovery_after[0], wrp->recovery_after[1], wrp->recovery_after[2],
		wrp->recovery_after[3], wrp->recovery_after[4], wrp->recovery_after[5]);
	spin_unlock_irqrestore(&wrp->irq_lock, flags);
	mutex_unlock(&wrp->phase_lock);
	return len;
}

static ssize_t recover_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct mt6580_pwrap *wrp = dev_get_drvdata(dev);
	unsigned int value;
	unsigned long flags;
	int ret = kstrtouint(buf, 0, &value);

	if (ret || (value != 1 && value != 2))
		return -EINVAL;
	mutex_lock(&wrp->phase_lock);
	if (wrp->stopping || wrp->suspend_phase || wrp->bank_active ||
	    system_state != SYSTEM_RUNNING) {
		ret = -EBUSY;
	} else {
		/* Permit an explicit retry of a latched failure; normal clients
		 * remain blocked until the worker publishes a healthy link.
		 */
		wrp->force_rebuild = value == 2;
		spin_lock_irqsave(&wrp->bus_lock, flags);
		spin_lock(&wrp->irq_lock);
		wrp->fault_pending = true;
		spin_unlock(&wrp->irq_lock);
		spin_unlock_irqrestore(&wrp->bus_lock, flags);
		__pm_wakeup_event(wrp->recovery_ws, 1000);
		mod_delayed_work(system_freezable_wq, &wrp->recovery_work, 0);
	}
	mutex_unlock(&wrp->phase_lock);
	return ret ? ret : count;
}

static void mt6580_pwrap_stop(struct mt6580_pwrap *wrp)
{
	WRITE_ONCE(wrp->stopping, true);
	writel(0, wrp->base + PWRAP_INT_EN);
	if (wrp->irq > 0)
		synchronize_irq(wrp->irq);
	cancel_delayed_work_sync(&wrp->recovery_work);
	writel(0, wrp->base + PWRAP_INT_EN);
}

static void mt6580_pwrap_cleanup(void *data)
{
	struct mt6580_pwrap *wrp = data;

	mt6580_pwrap_stop(wrp);
	wakeup_source_unregister(wrp->recovery_ws);
}

static void mt6580_pwrap_put_top(void *data)
{
	put_device(data);
}

static struct device *mt6580_pwrap_get_top(struct platform_device *pdev)
{
	struct device_node *node;
	struct platform_device *top;
	int ret;

	node = of_parse_phandle(pdev->dev.of_node, "mediatek,topckgen", 0);
	if (!node)
		return ERR_PTR(-EINVAL);
	top = of_find_device_by_node(node);
	of_node_put(node);
	if (!top)
		return ERR_PTR(-EPROBE_DEFER);
	ret = devm_add_action_or_reset(&pdev->dev, mt6580_pwrap_put_top, &top->dev);
	if (ret)
		return ERR_PTR(ret);
	if (!device_is_bound(&top->dev))
		return ERR_PTR(-EPROBE_DEFER);
	if (!device_link_add(&pdev->dev, &top->dev, DL_FLAG_AUTOREMOVE_CONSUMER))
		return ERR_PTR(-EINVAL);
	return &top->dev;
}

static int mt6580_pwrap_probe(struct platform_device *pdev)
{
	bool native = of_device_is_compatible(pdev->dev.of_node, "mediatek,mt6580-pwrap");
	struct reset_control *reset = NULL;
	struct device *top = NULL;
	struct clk *spi = NULL, *ref = NULL;
	struct clk_bulk_data *clks;
	struct mt6580_pwrap *wrp;
	struct regmap *regmap;
	struct regmap_config config = mt6580_pwrap_regmap;
	unsigned int before[ARRAY_SIZE(vproc_snapshot_regs)], after[ARRAY_SIZE(vproc_snapshot_regs)];
	int ret;

	wrp = devm_kzalloc(&pdev->dev, sizeof(*wrp), GFP_KERNEL);
	if (!wrp)
		return -ENOMEM;
	wrp->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(wrp->base))
		return PTR_ERR(wrp->base);
	wrp->dev = &pdev->dev;
	wrp->irq = -1;
	INIT_DELAYED_WORK(&wrp->recovery_work, mt6580_pwrap_recovery_work);
	mutex_init(&wrp->phase_lock);
	spin_lock_init(&wrp->bus_lock);
	spin_lock_init(&wrp->irq_lock);
	spin_lock_init(&wrp->stats_lock);
	wrp->recovery_snapshot_error = -ENODATA;
	wrp->init_stage = "firmware";
	if (native) {
		reset = devm_reset_control_get_exclusive(&pdev->dev, "spi");
		if (IS_ERR(reset))
			return dev_err_probe(&pdev->dev, PTR_ERR(reset), "PWRAP SPI reset unavailable\n");
		top = mt6580_pwrap_get_top(pdev);
		if (IS_ERR(top))
			return dev_err_probe(&pdev->dev, PTR_ERR(top), "PWRAP TOPCKGEN unavailable\n");
		spi = devm_clk_get(&pdev->dev, "spi");
		if (IS_ERR(spi))
			return PTR_ERR(spi);
		ref = devm_clk_get(&pdev->dev, "26m");
		if (IS_ERR(ref))
			return PTR_ERR(ref);
	}
	/* Keep clocks for PMIC IRQ, regulator and final atomic power-off users.
	 * Older bring-up DTs have no clocks and retain firmware gate ownership.
	 */
	ret = devm_clk_bulk_get_all_enabled(&pdev->dev, &clks);
	if (ret < 0)
		return dev_err_probe(&pdev->dev, ret, "PWRAP clocks unavailable\n");
	/* Firmware can leave INT_EN=0x7ffffffd. Mask it before Linux takes
	 * ownership, including the retained-link path. Never expose an IRQ
	 * source without a registered handler.
	 */
	writel(0, wrp->base + PWRAP_INT_EN);
	wrp->top = top;
	wrp->reset = reset;
	wrp->spi = spi;
	wrp->ref = ref;
	wrp->firmware_initialized = !!(readl(wrp->base + WACS2_RDATA) & WACS_INIT_DONE);
	if (!native && !wrp->firmware_initialized)
		return dev_err_probe(&pdev->dev, -ENODEV, "firmware PWRAP is not initialized\n");
	config.lock_arg = wrp;
	regmap = devm_regmap_init(&pdev->dev, NULL, wrp, &config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);
	wrp->regmap = regmap;
	if (native && (force_init || !wrp->firmware_initialized)) {
		if (wrp->firmware_initialized) {
			ret = mt6580_pwrap_snapshot_vproc(wrp, before);
			if (ret)
				return dev_err_probe(&pdev->dev, ret, "pre-init VPROC snapshot failed\n");
		}
		ret = mt6580_pwrap_cold_init(wrp, top, reset, spi, ref);
		if (ret)
			return dev_err_probe(&pdev->dev, ret, "PWRAP initialization failed at %s (scan=%06x)\n",
					     wrp->init_stage, wrp->si_pass_mask);
		if (wrp->firmware_initialized) {
			ret = mt6580_pwrap_snapshot_vproc(wrp, after);
			if (!ret && memcmp(before, after, sizeof(before)))
				ret = -EIO;
			if (ret) {
				mt6580_pwrap_quiesce(wrp);
				return dev_err_probe(&pdev->dev, ret, "VPROC changed across serial-link initialization\n");
			}
		}
		dev_info(&pdev->dev, "kernel initialized link: scan=%06x sample=%u\n",
			 wrp->si_pass_mask, wrp->si_point);
	}
	ret = mt6580_pwrap_link_check(wrp);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "MT6350 DEW read test failed\n");
	dev_info(&pdev->dev, "link verified (DEW=%04x)\n", MT6350_READ_TEST_VALUE);
	wrp->recovery_ws = wakeup_source_register(&pdev->dev, "mt6580-pwrap-recovery");
	if (!wrp->recovery_ws)
		return -ENOMEM;
	ret = platform_get_irq_optional(pdev, 0);
	if (ret <= 0 && (native || (ret != -ENXIO && ret != -ENODATA))) {
		wakeup_source_unregister(wrp->recovery_ws);
		return ret ?: -ENXIO;
	}
	if (ret > 0) {
		wrp->irq = ret;
		ret = devm_request_irq(&pdev->dev, wrp->irq, mt6580_pwrap_irq, 0,
				       dev_name(&pdev->dev), wrp);
		if (ret) {
			wakeup_source_unregister(wrp->recovery_ws);
			return dev_err_probe(&pdev->dev, ret, "PWRAP IRQ unavailable\n");
		}
	}
	/* Registered after the IRQ so cleanup stops work before freeing it;
	 * children registered below are removed before this action runs.
	 */
	ret = devm_add_action_or_reset(&pdev->dev, mt6580_pwrap_cleanup, wrp);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, wrp);
	ret = devm_of_platform_populate(&pdev->dev);
	if (ret)
		return ret;
	WRITE_ONCE(wrp->online, true);
	if (wrp->irq > 0)
		writel(PWRAP_IRQ_MASK, wrp->base + PWRAP_INT_EN);
	if (readl(wrp->base + PWRAP_INT_RAW) & PWRAP_ERROR_MASK)
		mt6580_pwrap_request_recovery(wrp);
	return 0;
}

static const struct of_device_id mt6580_pwrap_match[] = {
	{ .compatible = "mediatek,mt6580-pwrap" },
	{ .compatible = "mediatek,mt6580-firmware-pwrap" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6580_pwrap_match);

static void mt6580_pwrap_remove(struct platform_device *pdev)
{
	mt6580_pwrap_stop(platform_get_drvdata(pdev));
}

static struct platform_driver mt6580_pwrap_driver = {
	.probe = mt6580_pwrap_probe,
	.remove = mt6580_pwrap_remove,
	.shutdown = mt6580_pwrap_remove,
	.driver = {
		.name = "mt6580-firmware-pwrap",
		.of_match_table = mt6580_pwrap_match,
		.dev_groups = mt6580_pwrap_groups,
	},
};

bool mt6580_pwrap_infra_ready(struct device *dev)
{
	struct mt6580_pwrap *wrp;
	u32 status;

	if (dev->driver != &mt6580_pwrap_driver.driver)
		return false;
	wrp = dev_get_drvdata(dev);
	status = readl(wrp->base + WACS2_RDATA);
	return READ_ONCE(wrp->suspend_phase) && (status & WACS_INIT_DONE) &&
		FIELD_GET(WACS_FSM, status) == WACS_IDLE &&
		!READ_ONCE(wrp->fault_pending) && !READ_ONCE(wrp->recovering) &&
		!READ_ONCE(wrp->failed);
}
EXPORT_SYMBOL_GPL(mt6580_pwrap_infra_ready);
module_platform_driver(mt6580_pwrap_driver);
MODULE_DESCRIPTION("MT6580 MT6350 PMIC wrapper");
MODULE_LICENSE("GPL");
