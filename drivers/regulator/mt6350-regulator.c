// SPDX-License-Identifier: GPL-2.0-only
/* MT6350 CPU and board LDO supplies. */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>

#define MT6350_VGP1_CON0	0x050a
#define MT6350_VGP1_CON1	0x0530
#define MT6350_VIO28_CON0	0x0500
#define MT6350_VUSB_CON0	0x0502
#define MT6350_VEMC_CON0	0x0508
#define MT6350_VEMC_CON1	0x052e
#define MT6350_VIO18_CON0	0x0556
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

/* These LDOs request enable through RG[14], but QI[15] reports whether
 * the rail is actually on. Preserve the unrelated LP/calibration fields.
 */
static int mt6350_ldo_is_enabled(struct regulator_dev *rdev)
{
	unsigned int val;
	int ret = regmap_read(rdev->regmap, rdev->desc->enable_reg, &val);

	return ret ? ret : !!(val & BIT(15));
}

static const struct regulator_ops mt6350_fixed_ldo_ops = {
	.list_voltage = regulator_list_voltage_linear,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = mt6350_ldo_is_enabled,
};

static const unsigned int mt6350_vemc_voltages[] = { 3000000, 3300000 };

static const struct regulator_ops mt6350_vemc_ops = {
	.list_voltage = regulator_list_voltage_table,
	.map_voltage = regulator_map_voltage_iterate,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = mt6350_ldo_is_enabled,
};

#define MT6350_FIXED_LDO(_name, _id, _reg, _uv) { \
	.name = #_name, .of_match = "ldo_" #_name, .id = _id, \
	.type = REGULATOR_VOLTAGE, .owner = THIS_MODULE, \
	.ops = &mt6350_fixed_ldo_ops, .n_voltages = 1, .min_uV = _uv, \
	.enable_reg = _reg, .enable_mask = BIT(14), .enable_time = 264, \
}

static const struct regulator_desc mt6350_board_ldos[] = {
	MT6350_FIXED_LDO(vusb, 2, MT6350_VUSB_CON0, 3300000),
	MT6350_FIXED_LDO(vio18, 3, MT6350_VIO18_CON0, 1800000),
	MT6350_FIXED_LDO(vio28, 4, MT6350_VIO28_CON0, 2800000),
	{
		.name = "vemc", .of_match = "ldo_vemc", .id = 5,
		.type = REGULATOR_VOLTAGE, .owner = THIS_MODULE,
		.ops = &mt6350_vemc_ops,
		.n_voltages = ARRAY_SIZE(mt6350_vemc_voltages),
		.volt_table = mt6350_vemc_voltages,
		.vsel_reg = MT6350_VEMC_CON1, .vsel_mask = BIT(7),
		.enable_reg = MT6350_VEMC_CON0, .enable_mask = BIT(14),
		.enable_time = 264,
	},
};

static int mt6350_regulator_probe(struct platform_device *pdev)
{
	struct regulator_config config = { .dev = &pdev->dev };
	struct regulator_dev *rdev;
	struct regulator_desc *vproc;
	unsigned int ctrl;
	int ret, i;

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
	if (IS_ERR(rdev))
		return PTR_ERR(rdev);

	for (i = 0; i < ARRAY_SIZE(mt6350_board_ldos); i++) {
		rdev = devm_regulator_register(&pdev->dev, &mt6350_board_ldos[i], &config);
		if (IS_ERR(rdev))
			return dev_err_probe(&pdev->dev, PTR_ERR(rdev),
					     "Failed to register %s\n", mt6350_board_ldos[i].name);
	}
	return 0;
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
MODULE_DESCRIPTION("MT6350 VPROC and board LDO regulators");
MODULE_LICENSE("GPL");
