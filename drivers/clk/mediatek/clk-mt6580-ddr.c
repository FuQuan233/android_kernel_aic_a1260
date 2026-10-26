// SPDX-License-Identifier: GPL-2.0-only
/* Read the firmware memory clock without changing live DDR configuration. */
#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/soc/mediatek/mt6580-ddr.h>

struct mt6580_ddr_clk {
	struct clk_hw hw;
	void __iomem *base[3];
};

static struct platform_driver mt6580_ddr_clk_driver;

int mt6580_ddr_sleep_snapshot(struct device *dev, u32 values[4])
{
	/* MT6580 BSP spm_golden_setting_cmp(): DDRPHY, not the ORed mirrors. */
	static const u32 golden[] = { 0x063c0000, 0, 0x0000fc10, 0x40101000 };
	struct mt6580_ddr_clk *ddr;
	int i, ret = 0;

	if (dev->driver != &mt6580_ddr_clk_driver.driver)
		return -ENODEV;
	ddr = dev_get_drvdata(dev);
	for (i = 0; i < ARRAY_SIZE(golden); i++) {
		u32 mask = i == 3 ? ~BIT(31) : ~0U;

		values[i] = readl(ddr->base[0] + 0x5c0 + i * 4);
		/* The BSP PCM clears/sets 0x5cc[31] in its APSRC routines.
		 * Keep that transition visible, checking all other bits exactly.
		 */
		if ((values[i] & mask) != golden[i])
			ret = -EINVAL;
	}
	return ret;
}
EXPORT_SYMBOL_GPL(mt6580_ddr_sleep_snapshot);

static u32 mt6580_ddr_read(struct mt6580_ddr_clk *ddr, u32 offset)
{
	/* The BSP combines the mirrored DRAMC AO/NAO and DDRPHY fields. */
	return readl(ddr->base[0] + offset) | readl(ddr->base[1] + offset) |
	       readl(ddr->base[2] + offset);
}

static unsigned long mt6580_ddr_recalc_rate(struct clk_hw *hw,
					  unsigned long parent_rate)
{
	struct mt6580_ddr_clk *ddr = container_of(hw, struct mt6580_ddr_clk, hw);
	u32 div = mt6580_ddr_read(ddr, 0x604) >> 25;
	u32 ncpo = mt6580_ddr_read(ddr, 0x624) >> 1;
	u32 fbdiv = (mt6580_ddr_read(ddr, 0x618) >> 2) & 0x7f;
	u32 factor;
	u64 rate;

	if (!div || !ncpo || !fbdiv)
		return 0;
	if ((mt6580_ddr_read(ddr, 0x640) & 3) == 3)
		factor = 1 << ((mt6580_ddr_read(ddr, 0x608) >> 10) & 3);
	else
		factor = 1 << (((mt6580_ddr_read(ddr, 0x60c) >> 10) & 3) + 2);

	/* BSP DDR data-rate formula; EMI1X is one quarter of that rate.
	 * Divide before the final multiplication to keep intermediates in u64.
	 */
	rate = div64_u64((u64)parent_rate * ncpo * factor, (u64)div << 24);
	return rate * fbdiv / 4;
}

static const struct clk_ops mt6580_ddr_ops = {
	.recalc_rate = mt6580_ddr_recalc_rate,
};

static int mt6580_ddr_clk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct clk_parent_data parent = { .fw_name = "xtal" };
	struct clk_init_data init = {
		.name = "mt6580_emi_memory_src",
		.ops = &mt6580_ddr_ops,
		.parent_data = &parent,
		.num_parents = 1,
		.flags = CLK_IS_CRITICAL | CLK_GET_RATE_NOCACHE,
	};
	struct mt6580_ddr_clk *ddr;
	int i, ret;

	ddr = devm_kzalloc(dev, sizeof(*ddr), GFP_KERNEL);
	if (!ddr)
		return -ENOMEM;
	for (i = 0; i < ARRAY_SIZE(ddr->base); i++) {
		ddr->base[i] = devm_platform_ioremap_resource(pdev, i);
		if (IS_ERR(ddr->base[i]))
			return PTR_ERR(ddr->base[i]);
	}
	if (!mt6580_ddr_recalc_rate(&ddr->hw, 26000000))
		return dev_err_probe(dev, -EINVAL, "Invalid firmware memory PLL setup\n");
	platform_set_drvdata(pdev, ddr);
	ddr->hw.init = &init;
	ret = devm_clk_hw_register(dev, &ddr->hw);
	if (ret)
		return ret;
	return devm_of_clk_add_hw_provider(dev, of_clk_hw_simple_get, &ddr->hw);
}

static const struct of_device_id mt6580_ddr_clk_match[] = {
	{ .compatible = "mediatek,mt6580-ddrphy-clock" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6580_ddr_clk_match);

static struct platform_driver mt6580_ddr_clk_driver = {
	.probe = mt6580_ddr_clk_probe,
	.driver = {
		.name = "mt6580-ddr-clocks",
		.of_match_table = mt6580_ddr_clk_match,
	},
};
module_platform_driver(mt6580_ddr_clk_driver);
MODULE_DESCRIPTION("MT6580 firmware memory clock readback");
MODULE_LICENSE("GPL");
