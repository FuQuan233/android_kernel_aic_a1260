// SPDX-License-Identifier: GPL-2.0-only
/* USB gates from the MT6580 vendor clock manager. */
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include <dt-bindings/clock/mt6580-clk.h>

static DEFINE_SPINLOCK(mt6580_usb_clk_lock);

static int mt6580_usb_clk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct clk_hw_onecell_data *data;
	struct clk_hw *source;
	void __iomem *base;
	bool top = of_device_is_compatible(dev->of_node, "mediatek,mt6580-topckgen");
	unsigned int count = top ? 1 : 2;
	unsigned int i;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);
	data = devm_kzalloc(dev, struct_size(data, hws, count), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->num = count;

	if (top) {
		/* CLK_GATING_CTRL1[13]: 1 gates the USB bus clock. */
		data->hws[CLK_MT6580_TOP_USB] = devm_clk_hw_register_gate(dev,
			"mt6580_usb_bus", NULL, 0, base + 0x24, 13,
			CLK_GATE_SET_TO_DISABLE, &mt6580_usb_clk_lock);
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
	{ }
};
MODULE_DEVICE_TABLE(of, mt6580_usb_clk_match);

static struct platform_driver mt6580_usb_clk_driver = {
	.probe = mt6580_usb_clk_probe,
	.driver = {
		.name = "mt6580-usb-clocks",
		.of_match_table = mt6580_usb_clk_match,
	},
};
module_platform_driver(mt6580_usb_clk_driver);

MODULE_DESCRIPTION("MT6580 USB clock gates");
MODULE_LICENSE("GPL");
