// SPDX-License-Identifier: GPL-2.0-only
/* MT6580 WACS2 access, retaining the bootloader's MT6350 wrapper setup. */
#include <linux/bitfield.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define WACS2_CMD	0x9c
#define WACS2_RDATA	0xa0
#define WACS2_VLDCLR	0xa4
#define WACS_FSM	GENMASK(18, 16)
#define WACS_IDLE	0
#define WACS_VALID	6
#define WACS_INIT_DONE	BIT(21)

struct mt6580_pwrap {
	void __iomem *base;
};

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
	ret = readl_poll_timeout(wrp->base + WACS2_RDATA, status,
				FIELD_GET(WACS_FSM, status) == WACS_IDLE, 10, 10000);
	if (ret)
		return ret;

	writel((write ? BIT(31) : 0) | ((reg >> 1) << 16) |
	       (write ? *value : 0), wrp->base + WACS2_CMD);
	if (write)
		return readl_poll_timeout(wrp->base + WACS2_RDATA, status,
				FIELD_GET(WACS_FSM, status) == WACS_IDLE, 10, 10000);
	ret = readl_poll_timeout(wrp->base + WACS2_RDATA, status,
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
	.reg_bits = 16,
	.val_bits = 16,
	.reg_stride = 2,
	.max_register = 0xfffe,
	.reg_read = mt6580_pwrap_read,
	.reg_write = mt6580_pwrap_write,
};

static int mt6580_pwrap_probe(struct platform_device *pdev)
{
	struct mt6580_pwrap *wrp;
	struct regmap *regmap;

	wrp = devm_kzalloc(&pdev->dev, sizeof(*wrp), GFP_KERNEL);
	if (!wrp)
		return -ENOMEM;
	wrp->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(wrp->base))
		return PTR_ERR(wrp->base);
	if (!(readl(wrp->base + WACS2_RDATA) & WACS_INIT_DONE))
		return dev_err_probe(&pdev->dev, -ENODEV, "firmware PWRAP is not initialized\n");
	regmap = devm_regmap_init(&pdev->dev, NULL, wrp, &mt6580_pwrap_regmap);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);
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
	},
};
module_platform_driver(mt6580_pwrap_driver);
MODULE_DESCRIPTION("MT6580 firmware-initialized PMIC wrapper");
MODULE_LICENSE("GPL");
