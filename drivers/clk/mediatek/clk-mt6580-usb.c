// SPDX-License-Identifier: GPL-2.0-only
/* Peripheral and GPU clocks from the MT6580 vendor clock manager. */
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include "clk-gate.h"
#include "clk-mtk.h"

#include <dt-bindings/clock/mt6580-clk.h>

static DEFINE_SPINLOCK(mt6580_usb_clk_lock);

static const struct mtk_gate_regs mt6580_mfg_regs = {
	.sta_ofs = 0, .set_ofs = 4, .clr_ofs = 8,
};

static const struct mtk_gate mt6580_mfg_clks[] = {
	GATE_MTK(CLK_MT6580_MFG_BG3D, "mt6580_bg3d", "mt6580_mfg_bus",
		 &mt6580_mfg_regs, 0, &mtk_clk_gate_ops_setclr),
};

static const struct mtk_clk_desc mt6580_mfg_desc = {
	.clks = mt6580_mfg_clks,
	.num_clks = ARRAY_SIZE(mt6580_mfg_clks),
};

/* Preserve firmware muxes; no DVFS or voltage changes during bring-up. */
static unsigned long mt6580_mfg_rate(void __iomem *base)
{
	u32 mux = (readl(base + 4) >> 8) & 0x3f;

	switch (mux & 0x38) {
	case 0x08:
		return 416000000;
	case 0x10:
		return 500500000;
	case 0x20:
		switch (mux & 7) {
		case 1: return 300300000;
		case 2: return 214500000;
		case 4: return 107250000;
		}
		break;
	}
	return 0;
}

static int mt6580_usb_clk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct clk_hw_onecell_data *data;
	struct clk_hw *source;
	void __iomem *base;
	bool top = of_device_is_compatible(dev->of_node, "mediatek,mt6580-topckgen");
	unsigned int count = 2;
	unsigned int i;

	if (of_device_is_compatible(dev->of_node, "mediatek,mt6580-mfgcfg"))
		return mtk_clk_simple_probe(pdev);
	if (top)
		count = 8;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);
	data = devm_kzalloc(dev, struct_size(data, hws, count), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->num = count;

	if (top) {
		/* Keep LK's mux selection; rates from the MT6580 clock manager. */
		static const unsigned long msdc_rates[] = {
			125130000, 150150000, 187690000, 178280000,
			214500000, 187690000, 26000000, 208000000,
		};
		u32 mux = (readl(base) >> 11) & 7;

		source = devm_clk_hw_register_fixed_rate(dev, "mt6580_msdc0_src",
						       NULL, 0, msdc_rates[mux]);
		if (IS_ERR(source))
			return PTR_ERR(source);
		data->hws[CLK_MT6580_TOP_MSDC0] = devm_clk_hw_register_gate(dev,
			"mt6580_msdc0", "mt6580_msdc0_src", 0, base + 0x20, 17,
			CLK_GATE_SET_TO_DISABLE, &mt6580_usb_clk_lock);
		dev_info(dev, "MSDC0 firmware mux %u, source %lu Hz\n",
			 mux, msdc_rates[mux]);

		/* CLK_GATING_CTRL1[13]: 1 gates the USB bus clock. */
		data->hws[CLK_MT6580_TOP_USB] = devm_clk_hw_register_gate(dev,
			"mt6580_usb_bus", NULL, 0, base + 0x24, 13,
			CLK_GATE_SET_TO_DISABLE, &mt6580_usb_clk_lock);

		/* Vendor I2C source is 125 MHz, divided by ten in the controller. */
		source = devm_clk_hw_register_fixed_rate(dev, "mt6580_i2c_src",
						       NULL, 0, 125000000);
		if (IS_ERR(source))
			return PTR_ERR(source);
		data->hws[CLK_MT6580_TOP_I2C1] = devm_clk_hw_register_gate(dev,
			"mt6580_i2c1", "mt6580_i2c_src", 0, base + 0x24, 4,
			CLK_GATE_SET_TO_DISABLE, &mt6580_usb_clk_lock);
		data->hws[CLK_MT6580_TOP_I2C0] = devm_clk_hw_register_gate(dev,
			"mt6580_i2c0", "mt6580_i2c_src", 0, base + 0x24, 3,
			CLK_GATE_SET_TO_DISABLE, &mt6580_usb_clk_lock);
		data->hws[CLK_MT6580_TOP_APDMA] = devm_clk_hw_register_gate(dev,
			"mt6580_apdma", "mt6580_i2c_src", 0, base + 0x24, 2,
			CLK_GATE_SET_TO_DISABLE, &mt6580_usb_clk_lock);
		/* CLK_MUX_SEL0[18]: 26 MHz or UPLL/12; preserve the LK mux. */
		source = devm_clk_hw_register_fixed_rate(dev, "mt6580_disp_pwm_src",
			NULL, 0, readl(base) & BIT(18) ? 104000000 : 26000000);
		if (IS_ERR(source))
			return PTR_ERR(source);
		data->hws[CLK_MT6580_TOP_DISP_PWM] = devm_clk_hw_register_gate(dev,
			"mt6580_disp_pwm", "mt6580_disp_pwm_src", 0, base + 0x20, 0,
			CLK_GATE_SET_TO_DISABLE, &mt6580_usb_clk_lock);

		/* MFG power-on needs MPLL_D5 enabled before the top bus gate. */
		source = devm_clk_hw_register_fixed_rate(dev, "mt6580_mpll_d5_src",
						       NULL, 0, 300300000);
		if (IS_ERR(source))
			return PTR_ERR(source);
		data->hws[CLK_MT6580_TOP_MPLL_D5] = devm_clk_hw_register_gate(dev,
			"mt6580_mpll_d5", "mt6580_mpll_d5_src", 0, base + 0x30, 2,
			0, &mt6580_usb_clk_lock);
		source = devm_clk_hw_register_fixed_rate(dev, "mt6580_mfg_src",
						       NULL, 0, mt6580_mfg_rate(base));
		if (IS_ERR(source))
			return PTR_ERR(source);
		data->hws[CLK_MT6580_TOP_MFG] = devm_clk_hw_register_gate(dev,
			"mt6580_mfg_bus", "mt6580_mfg_src", 0, base + 0x20, 2,
			CLK_GATE_SET_TO_DISABLE, &mt6580_usb_clk_lock);
		dev_info(dev, "MFG firmware mux %#x, source %lu Hz\n",
			 (readl(base + 4) >> 8) & 0x3f, mt6580_mfg_rate(base));
	} else {
		/* Do not invent PLL programming before its rates are ported. */
		if (!(readl(base + 0x140) & BIT(0)))
			return dev_err_probe(dev, -EIO,
					     "UNIVPLL is off; firmware clock setup required\n");
		source = devm_clk_hw_register_fixed_rate(dev, "mt6580_usb_48m_src",
						       NULL, 0, 48000000);
		if (IS_ERR(source))
			return PTR_ERR(source);
		/* Vendor UNIV48M_EN_BIT[29] and USB48M_EN_BIT[28]. */
		data->hws[CLK_MT6580_APMIXED_UNIV48M] = devm_clk_hw_register_gate(dev,
			"mt6580_univ48m", "mt6580_usb_48m_src", 0,
			base + 0x140, 29, 0, &mt6580_usb_clk_lock);
		data->hws[CLK_MT6580_APMIXED_USB48M] = devm_clk_hw_register_gate(dev,
			"mt6580_usb48m", "mt6580_usb_48m_src", 0,
			base + 0x140, 28, 0, &mt6580_usb_clk_lock);
	}
	for (i = 0; i < count; i++)
		if (IS_ERR(data->hws[i]))
			return PTR_ERR(data->hws[i]);

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, data);
}

static const struct of_device_id mt6580_usb_clk_match[] = {
	{ .compatible = "mediatek,mt6580-topckgen" },
	{ .compatible = "mediatek,mt6580-apmixedsys" },
	{ .compatible = "mediatek,mt6580-mfgcfg", .data = &mt6580_mfg_desc },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6580_usb_clk_match);

static void mt6580_usb_clk_remove(struct platform_device *pdev)
{
	if (of_device_is_compatible(pdev->dev.of_node, "mediatek,mt6580-mfgcfg"))
		mtk_clk_simple_remove(pdev);
}

static struct platform_driver mt6580_usb_clk_driver = {
	.probe = mt6580_usb_clk_probe,
	.remove = mt6580_usb_clk_remove,
	.driver = {
		.name = "mt6580-usb-clocks",
		.of_match_table = mt6580_usb_clk_match,
	},
};
module_platform_driver(mt6580_usb_clk_driver);

MODULE_DESCRIPTION("MT6580 peripheral and GPU clocks");
MODULE_LICENSE("GPL");
