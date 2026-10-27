// SPDX-License-Identifier: GPL-2.0-only
/* MT6580 WACS2 access, retaining the bootloader's MT6350 wrapper setup. */
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mfd/mt6350/registers.h>
#include <linux/mutex.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/panic.h>
#include <linux/regmap.h>
#include <linux/soc/mediatek/mt6580-pwrap.h>
#include <linux/spinlock.h>

#define WACS2_CMD	0x9c
#define WACS2_RDATA	0xa0
#define WACS2_VLDCLR	0xa4
#define WACS_FSM	GENMASK(18, 16)
#define WACS_IDLE	0
#define WACS_VALID	6
#define WACS_INIT_DONE	BIT(21)
#define MT6350_DEW_READ_TEST	0x18c
#define MT6350_READ_TEST_VALUE	0x5aa5

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

struct mt6580_pwrap {
	void __iomem *base;
	struct regmap *regmap;
	struct mutex phase_lock;
	spinlock_t stats_lock;
	struct mt6580_pwrap_link_stats link;
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

static int mt6580_pwrap_link_check(struct mt6580_pwrap *wrp)
{
	unsigned int value = 0;
	unsigned long flags;
	int ret;

	/* Fixed, side-effect-free DEW signature, also used by the MT6580 BSP. */
	ret = regmap_read(wrp->regmap, MT6350_DEW_READ_TEST, &value);
	if (!ret && value != MT6350_READ_TEST_VALUE)
		ret = -EIO;
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
	if (wrp->suspend_phase || wrp->bank_active) {
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

	mt6580_pwrap_link_check(wrp);
	spin_lock_irqsave(&wrp->stats_lock, flags);
	link = wrp->link;
	spin_unlock_irqrestore(&wrp->stats_lock, flags);
	status = readl(wrp->base + WACS2_RDATA);
	return sysfs_emit(buf,
		"init_done=%u wacs2=%08x\n"
		"read_test=%04x expected=%04x error=%d checks=%u failures=%u\n"
		"reads=%u writes=%u errors=%u timeouts=%u init_lost=%u stale_valid_clears=%u\n"
		"last_error=%d reg=%04x write=%u stage=%u status=%08x\n",
		!!(status & WACS_INIT_DONE), status,
		link.test_value, MT6350_READ_TEST_VALUE, link.test_error,
		link.checks, link.check_errors, link.reads, link.writes,
		link.errors, link.timeouts, link.init_lost, link.stale_valid_clears,
		link.last_error, link.last_reg, link.last_write,
		link.last_stage, link.last_status);
}
static DEVICE_ATTR_RO(link_state);

static struct attribute *mt6580_pwrap_attrs[] = {
	&dev_attr_spm_phase.attr,
	&dev_attr_sleep_voltage.attr,
	&dev_attr_link_state.attr,
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
	if (clear_stale && (status & WACS_INIT_DONE) &&
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
			!(*status & WACS_INIT_DONE) || FIELD_GET(WACS_FSM, *status) == fsm,
			10, 10000, false, wrp, clear_stale, clears);
	if (!(*status & WACS_INIT_DONE))
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

static int mt6580_pwrap_read(void *context, unsigned int reg, unsigned int *value)
{
	return mt6580_pwrap_xfer(context, reg, value, false);
}

static int mt6580_pwrap_write(void *context, unsigned int reg, unsigned int value)
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
};

static int mt6580_pwrap_probe(struct platform_device *pdev)
{
	struct clk_bulk_data *clks;
	struct mt6580_pwrap *wrp;
	struct regmap *regmap;
	int ret;

	wrp = devm_kzalloc(&pdev->dev, sizeof(*wrp), GFP_KERNEL);
	if (!wrp)
		return -ENOMEM;
	wrp->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(wrp->base))
		return PTR_ERR(wrp->base);
	mutex_init(&wrp->phase_lock);
	spin_lock_init(&wrp->stats_lock);
	/* Keep clocks for PMIC IRQ, regulator and final atomic power-off users.
	 * Older bring-up DTs have no clocks and retain firmware gate ownership.
	 */
	ret = devm_clk_bulk_get_all_enabled(&pdev->dev, &clks);
	if (ret < 0)
		return dev_err_probe(&pdev->dev, ret, "PWRAP clocks unavailable\n");
	if (!(readl(wrp->base + WACS2_RDATA) & WACS_INIT_DONE))
		return dev_err_probe(&pdev->dev, -ENODEV, "firmware PWRAP is not initialized\n");
	regmap = devm_regmap_init(&pdev->dev, NULL, wrp, &mt6580_pwrap_regmap);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);
	wrp->regmap = regmap;
	ret = mt6580_pwrap_link_check(wrp);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "MT6350 DEW read test failed\n");
	dev_info(&pdev->dev, "firmware link verified (DEW=%04x)\n", MT6350_READ_TEST_VALUE);
	platform_set_drvdata(pdev, wrp);
	return devm_of_platform_populate(&pdev->dev);
}

static const struct of_device_id mt6580_pwrap_match[] = {
	{ .compatible = "mediatek,mt6580-firmware-pwrap" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6580_pwrap_match);

static struct platform_driver mt6580_pwrap_driver = {
	.probe = mt6580_pwrap_probe,
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
		FIELD_GET(WACS_FSM, status) == WACS_IDLE;
}
EXPORT_SYMBOL_GPL(mt6580_pwrap_infra_ready);
module_platform_driver(mt6580_pwrap_driver);
MODULE_DESCRIPTION("MT6580 firmware-initialized PMIC wrapper");
MODULE_LICENSE("GPL");
