// SPDX-License-Identifier: GPL-2.0-only
/* MT6350 VGP1, used by the A1260 FT6336U touchscreen. */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>

#define MT6350_VGP1_CON0	0x050a
#define MT6350_VGP1_CON1	0x0530

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

	config.regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!config.regmap)
		return -ENODEV;
	rdev = devm_regulator_register(&pdev->dev, &mt6350_vgp1_desc, &config);
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
MODULE_DESCRIPTION("MT6350 VGP1 regulator");
MODULE_LICENSE("GPL");
