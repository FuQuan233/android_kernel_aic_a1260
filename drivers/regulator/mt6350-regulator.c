// SPDX-License-Identifier: GPL-2.0-only
/* MT6350 CPU supply and VGP1 touchscreen supply. */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>

#define MT6350_VGP1_CON0	0x050a
#define MT6350_VGP1_CON1	0x0530
#define MT6350_VPROC_CTRL	0x0216
#define MT6350_VPROC_EN	0x021a
#define MT6350_VPROC_VOSEL	0x021e
#define MT6350_VPROC_VOSEL_ON	0x0220
#define MT6350_VPROC_VOSEL_ACTUAL	0x0224

static int mt6350_vproc_get_voltage_sel(struct regulator_dev *rdev)
{
	unsigned int val;
	int ret = regmap_read(rdev->regmap, MT6350_VPROC_VOSEL_ACTUAL, &val);

	return ret ? ret : val & 0x7f;
}

static int mt6350_vproc_voltage_time(struct regulator_dev *rdev,
				     unsigned int old, unsigned int new)
{
	if (old == new)
		return 0;
	/* BSP: 12.5 mV/us up, 3.125 mV/us down, plus command margin. */
	return max(25U, new > old ? DIV_ROUND_UP(new - old, 2) + 5 :
		   (old - new) * 2 + 5);
}

static const struct regulator_ops mt6350_vproc_ops = {
	.list_voltage = regulator_list_voltage_linear,
	.map_voltage = regulator_map_voltage_linear,
	.get_voltage_sel = mt6350_vproc_get_voltage_sel,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.set_voltage_time_sel = mt6350_vproc_voltage_time,
};

static const struct regulator_desc mt6350_vproc_desc = {
	.name = "vproc",
	.of_match = "buck_vproc",
	.id = 1,
	.type = REGULATOR_VOLTAGE,
	.owner = THIS_MODULE,
	.ops = &mt6350_vproc_ops,
	.n_voltages = 128,
	.min_uV = 700000,
	.uV_step = 6250,
	.vsel_reg = MT6350_VPROC_VOSEL,
	.vsel_mask = GENMASK(6, 0),
	.enable_reg = MT6350_VPROC_EN,
	.enable_mask = BIT(13), /* Actual enable state, not the control request. */
};

static const unsigned int mt6350_vgp1_voltages[] = {
	1200000, 1300000, 1500000, 1800000, 2000000, 2800000, 3000000, 3300000,
};

static const struct regulator_ops mt6350_vgp1_ops = {
	.list_voltage = regulator_list_voltage_table,
	.map_voltage = regulator_map_voltage_iterate,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
};

static const struct regulator_desc mt6350_vgp1_desc = {
	.name = "vgp1",
	.of_match = "ldo_vgp1",
	.id = 0,
	.type = REGULATOR_VOLTAGE,
	.owner = THIS_MODULE,
	.ops = &mt6350_vgp1_ops,
	.n_voltages = ARRAY_SIZE(mt6350_vgp1_voltages),
	.volt_table = mt6350_vgp1_voltages,
	.vsel_reg = MT6350_VGP1_CON1,
	.vsel_mask = GENMASK(7, 5),
	.enable_reg = MT6350_VGP1_CON0,
	.enable_mask = BIT(15),
	.enable_time = 264,
};

static int mt6350_regulator_probe(struct platform_device *pdev)
{
	struct regulator_config config = { .dev = &pdev->dev };
	struct regulator_dev *rdev;
	struct regulator_desc *vproc;
	unsigned int ctrl;
	int ret;

	config.regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!config.regmap)
		return -ENODEV;
	rdev = devm_regulator_register(&pdev->dev, &mt6350_vgp1_desc, &config);
	if (IS_ERR(rdev))
		return PTR_ERR(rdev);

	vproc = devm_kmemdup(&pdev->dev, &mt6350_vproc_desc, sizeof(*vproc), GFP_KERNEL);
	if (!vproc)
		return -ENOMEM;
	ret = regmap_read(config.regmap, MT6350_VPROC_CTRL, &ctrl);
	if (ret)
		return ret;
	/* Respect firmware's software/hardware voltage control selection. */
	if (ctrl & BIT(1))
		vproc->vsel_reg = MT6350_VPROC_VOSEL_ON;
	rdev = devm_regulator_register(&pdev->dev, vproc, &config);
	return PTR_ERR_OR_ZERO(rdev);
}

static const struct of_device_id mt6350_regulator_match[] = {
	{ .compatible = "mediatek,mt6350-regulator" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6350_regulator_match);

static struct platform_driver mt6350_regulator_driver = {
	.probe = mt6350_regulator_probe,
	.driver = {
		.name = "mt6350-regulator",
		.of_match_table = mt6350_regulator_match,
	},
};
module_platform_driver(mt6350_regulator_driver);
MODULE_DESCRIPTION("MT6350 VPROC and VGP1 regulators");
MODULE_LICENSE("GPL");
