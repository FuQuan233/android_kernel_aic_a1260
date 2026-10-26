// SPDX-License-Identifier: GPL-2.0-only
/* MT6580 WACS2 access, retaining the bootloader's MT6350 wrapper setup. */
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mfd/mt6350/registers.h>
#include <linux/mutex.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/soc/mediatek/mt6580-pwrap.h>

#define WACS2_CMD	0x9c
#define WACS2_RDATA	0xa0
#define WACS2_VLDCLR	0xa4
#define WACS_FSM	GENMASK(18, 16)
#define WACS_IDLE	0
#define WACS_VALID	6
#define WACS_INIT_DONE	BIT(21)

/* PCM's AUXADC workaround uses slots 4/5, not the normal VCORE slot 0. */
#define DVFS_ADR4	0x104
#define DVFS_WDATA4	0x108
#define DVFS_ADR5	0x10c
#define DVFS_WDATA5	0x110
#define AUXADC_START	BIT(4)

static const u16 suspend_slot_regs[] = {
	DVFS_ADR4, DVFS_WDATA4, DVFS_ADR5, DVFS_WDATA5,
};

struct mt6580_pwrap {
	void __iomem *base;
	struct regmap *regmap;
	struct mutex phase_lock;
	u32 saved_slots[ARRAY_SIZE(suspend_slot_regs)];
	u32 suspend_slots[ARRAY_SIZE(suspend_slot_regs)];
	unsigned int prepared, restored;
	bool suspend_phase;
};

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
 * Staging commands does not execute them or change the PMIC register.
 * WACS2 voltage/RTC users do not use these autonomous command slots.
 */
int mt6580_pwrap_prepare_suspend(struct device *dev)
{
	struct mt6580_pwrap *wrp = dev_get_drvdata(dev);
	unsigned int value;
	int i, ret;

	mutex_lock(&wrp->phase_lock);
	if (wrp->suspend_phase) {
		ret = -EBUSY;
		goto out;
	}
	ret = regmap_read(wrp->regmap, MT6350_STRUP_CON10, &value);
	if (ret)
		goto out;
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
out:
	mutex_unlock(&wrp->phase_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mt6580_pwrap_prepare_suspend);

int mt6580_pwrap_finish_suspend(struct device *dev)
{
	struct mt6580_pwrap *wrp = dev_get_drvdata(dev);
	int ret = 0;

	mutex_lock(&wrp->phase_lock);
	if (wrp->suspend_phase)
		ret = mt6580_pwrap_restore_slots(wrp);
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
			 "staged=%08x/%08x/%08x/%08x\n",
			 wrp->suspend_phase, wrp->prepared, wrp->restored,
			 wrp->saved_slots[0], wrp->saved_slots[1],
			 wrp->saved_slots[2], wrp->saved_slots[3],
			 wrp->suspend_slots[0], wrp->suspend_slots[1],
			 wrp->suspend_slots[2], wrp->suspend_slots[3]);
	mutex_unlock(&wrp->phase_lock);
	return len;
}
static DEVICE_ATTR_RO(spm_phase);

static struct attribute *mt6580_pwrap_attrs[] = {
	&dev_attr_spm_phase.attr,
	NULL,
};
ATTRIBUTE_GROUPS(mt6580_pwrap);

/* regmap serializes all users, including RTC reboot and regulator accesses. */
static int mt6580_pwrap_xfer(struct mt6580_pwrap *wrp, unsigned int reg,
			   unsigned int *value, bool write)
{
	u32 status;
	int ret;

	status = readl(wrp->base + WACS2_RDATA);
	if (!(status & WACS_INIT_DONE))
		return -ENODEV;
	if (FIELD_GET(WACS_FSM, status) == WACS_VALID)
		writel(1, wrp->base + WACS2_VLDCLR);
	ret = readl_poll_timeout_atomic(wrp->base + WACS2_RDATA, status,
				FIELD_GET(WACS_FSM, status) == WACS_IDLE, 10, 10000);
	if (ret)
		return ret;

	writel((write ? BIT(31) : 0) | ((reg >> 1) << 16) |
	       (write ? *value : 0), wrp->base + WACS2_CMD);
	if (write)
		return readl_poll_timeout_atomic(wrp->base + WACS2_RDATA, status,
				FIELD_GET(WACS_FSM, status) == WACS_IDLE, 10, 10000);
	ret = readl_poll_timeout_atomic(wrp->base + WACS2_RDATA, status,
				FIELD_GET(WACS_FSM, status) == WACS_VALID, 10, 10000);
	if (ret)
		return ret;
	*value = status & 0xffff;
	writel(1, wrp->base + WACS2_VLDCLR);
	return 0;
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
module_platform_driver(mt6580_pwrap_driver);
MODULE_DESCRIPTION("MT6580 firmware-initialized PMIC wrapper");
MODULE_LICENSE("GPL");
