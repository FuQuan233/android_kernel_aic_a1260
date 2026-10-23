// SPDX-License-Identifier: GPL-2.0-only
/* CPU, peripheral and GPU clocks from the MT6580 vendor clock manager. */
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include "clk-gate.h"
#include "clk-mtk.h"

#include <dt-bindings/clock/mt6580-clk.h>

static DEFINE_SPINLOCK(mt6580_usb_clk_lock);

struct mt6580_cpu_clk {
	struct clk_hw hw;
	void __iomem *pll;
	void __iomem *infra;
};

/* BSP level-0 OPPs. The cluster shares one ARMPLL and divider. */
static const struct {
	unsigned long rate;
	u32 pcw;
	u32 div;
} mt6580_cpu_rates[] = {
	{ 604500000, 0x000ba000, 10 }, /* ARMPLL 1209 MHz, CPU divide by two */
	{ 754000000, 0x010e8000, 8 },
	{ 903500000, 0x01116000, 8 },
	{ 1001000000, 0x0009a000, 8 },
	{ 1105000000, 0x000aa000, 8 },
	{ 1209000000, 0x000ba000, 8 },
	{ 1300000000, 0x000c8000, 8 },
};

static unsigned long mt6580_cpu_recalc_rate(struct clk_hw *hw,
					   unsigned long parent_rate)
{
	struct mt6580_cpu_clk *cpu = container_of(hw, struct mt6580_cpu_clk, hw);
	u32 con = readl(cpu->pll + 0x104);
	u32 div = readl(cpu->infra + 8) & 0x1f;
	u64 rate = (u64)(con & GENMASK(20, 0)) * 26000000;

	rate >>= 14 + ((con >> 24) & 7);
	if (div >= 9 && div <= 11)
		rate = rate * (12 - div) / 4;
	else if (div >= 17 && div <= 20)
		rate = rate * (21 - div) / 5;
	else if (div >= 25 && div <= 29)
		rate = rate * (30 - div) / 6;
	return rate;
}

static long mt6580_cpu_round_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long *parent_rate)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mt6580_cpu_rates) - 1; i++)
		if (rate <= mt6580_cpu_rates[i].rate)
			break;
	return mt6580_cpu_rates[i].rate;
}

static int mt6580_cpu_set_rate(struct clk_hw *hw, unsigned long rate,
			       unsigned long parent_rate)
{
	struct mt6580_cpu_clk *cpu = container_of(hw, struct mt6580_cpu_clk, hw);
	unsigned long flags;
	u32 mux, div, gate, con;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mt6580_cpu_rates); i++)
		if (rate == mt6580_cpu_rates[i].rate)
			break;
	if (i == ARRAY_SIZE(mt6580_cpu_rates))
		return -EINVAL;

	/* Follow the BSP MAINPLL handover; never reprogram the live CPU PLL.
	 * cpufreq-dt raises VPROC before increasing the clock and lowers it
	 * after decreasing the clock. All OPPs meet MAINPLL's 1.15 V minimum.
	 * Preserve unrelated mux, gate and divider bits, including firmware DCM.
	 */
	spin_lock_irqsave(&mt6580_usb_clk_lock, flags);
	mux = readl(cpu->infra);
	div = readl(cpu->infra + 8);
	gate = readl(cpu->infra + 0xa00);
	writel(gate & ~BIT(1), cpu->infra + 0xa00);
	writel((mux & ~GENMASK(3, 2)) | (3 << 2), cpu->infra);
	readl(cpu->infra);
	con = readl(cpu->pll + 0x104);
	con &= ~(GENMASK(26, 24) | GENMASK(20, 0));
	writel(con | mt6580_cpu_rates[i].pcw | BIT(31), cpu->pll + 0x104);
	udelay(20);
	writel((div & ~GENMASK(4, 0)) | mt6580_cpu_rates[i].div, cpu->infra + 8);
	writel((mux & ~GENMASK(3, 2)) | BIT(2), cpu->infra);
	readl(cpu->infra);
	writel(gate, cpu->infra + 0xa00);
	spin_unlock_irqrestore(&mt6580_usb_clk_lock, flags);
	return 0;
}

static const struct clk_ops mt6580_cpu_ops = {
	.recalc_rate = mt6580_cpu_recalc_rate,
	.round_rate = mt6580_cpu_round_rate,
	.set_rate = mt6580_cpu_set_rate,
};

static struct clk_hw *mt6580_register_cpu_clk(struct platform_device *pdev,
					    void __iomem *base)
{
	struct device *dev = &pdev->dev;
	struct mt6580_cpu_clk *cpu;
	struct clk_init_data init = {
		.name = "mt6580_cpu",
		.ops = &mt6580_cpu_ops,
		.flags = CLK_IS_CRITICAL | CLK_GET_RATE_NOCACHE,
	};
	int ret;

	cpu = devm_kzalloc(dev, sizeof(*cpu), GFP_KERNEL);
	if (!cpu)
		return ERR_PTR(-ENOMEM);
	cpu->pll = base;
	cpu->infra = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(cpu->infra))
		return ERR_CAST(cpu->infra);
	cpu->hw.init = &init;
	ret = devm_clk_hw_register(dev, &cpu->hw);
	return ret ? ERR_PTR(ret) : &cpu->hw;
}

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
	.need_runtime_pm = true,
};

/* Firmware PLL rates are retained; this driver does not retune MPLL/UPLL.
 * Only D5's divider gate is managed here. Other divider gates remain in the
 * firmware state, but their parent relationships must propagate D5 references.
 */
struct mt6580_d5_clk {
	struct clk_hw hw;
	void __iomem *base;
};

static int mt6580_d5_enable(struct clk_hw *hw)
{
	struct mt6580_d5_clk *d5 = container_of(hw, struct mt6580_d5_clk, hw);

	writel(BIT(2), d5->base + 0x60); /* SET_MPLL_FREDIV_EN */
	return 0;
}

static void mt6580_d5_disable(struct clk_hw *hw)
{
	struct mt6580_d5_clk *d5 = container_of(hw, struct mt6580_d5_clk, hw);

	writel(BIT(2), d5->base + 0x90); /* CLR_MPLL_FREDIV_EN */
}

static int mt6580_d5_is_enabled(struct clk_hw *hw)
{
	struct mt6580_d5_clk *d5 = container_of(hw, struct mt6580_d5_clk, hw);

	return !!(readl(d5->base + 0x30) & BIT(2));
}

static const struct clk_ops mt6580_d5_ops = {
	.enable = mt6580_d5_enable,
	.disable = mt6580_d5_disable,
	.is_enabled = mt6580_d5_is_enabled,
};

static struct clk_hw *mt6580_register_d5(struct device *dev, void __iomem *base)
{
	const char *parent = "mt6580_mpll_d5_src";
	struct clk_init_data init = {
		.name = "mt6580_mpll_d5",
		.ops = &mt6580_d5_ops,
		.parent_names = &parent,
		.num_parents = 1,
	};
	struct mt6580_d5_clk *d5;
	int ret;

	d5 = devm_kzalloc(dev, sizeof(*d5), GFP_KERNEL);
	if (!d5)
		return ERR_PTR(-ENOMEM);
	d5->base = base;
	d5->hw.init = &init;
	ret = devm_clk_hw_register(dev, &d5->hw);
	return ret ? ERR_PTR(ret) : &d5->hw;
}

static const struct {
	const char *name;
	const char *parent;
	unsigned int div;
} mt6580_fw_factors[] = {
	{ "mt6580_mpll_d2", "mt6580_mpll", 2 },
	{ "mt6580_mpll_d3", "mt6580_mpll", 3 },
	{ "mt6580_mpll_d5_src", "mt6580_mpll", 5 },
	{ "mt6580_mpll_d7", "mt6580_mpll", 7 },
	{ "mt6580_mpll_d11", "mt6580_mpll", 11 },
	{ "mt6580_mpll_d4", "mt6580_mpll_d2", 2 },
	{ "mt6580_mpll_d6", "mt6580_mpll_d3", 2 },
	{ "mt6580_mpll_d10", "mt6580_mpll_d5", 2 },
	{ "mt6580_mpll_d14", "mt6580_mpll_d7", 2 },
	{ "mt6580_mpll_d8", "mt6580_mpll_d4", 2 },
	{ "mt6580_mpll_d12", "mt6580_mpll_d6", 2 },
	{ "mt6580_mpll_d20", "mt6580_mpll_d10", 2 },
	{ "mt6580_mpll_d28", "mt6580_mpll_d14", 2 },
	{ "mt6580_mpll_d16", "mt6580_mpll_d8", 2 },
	{ "mt6580_mpll_d24", "mt6580_mpll_d12", 2 },
	{ "mt6580_mpll_d40", "mt6580_mpll_d20", 2 },
	{ "mt6580_upll_d3", "mt6580_upll", 3 },
	{ "mt6580_upll_d6", "mt6580_upll_d3", 2 },
	{ "mt6580_upll_d7", "mt6580_upll", 7 },
	{ "mt6580_upll_d12", "mt6580_upll_d6", 2 },
	{ "mt6580_upll_d16", "mt6580_upll", 16 },
};

static const char * const mt6580_msdc_parents[] = {
	"mt6580_mpll_d12", "mt6580_mpll_d10", "mt6580_mpll_d8",
	"mt6580_upll_d7", "mt6580_mpll_d7", "mt6580_mpll_d8",
	"dummy26m", "mt6580_upll_d6",
};

/* Snapshot the boot muxes: there is deliberately no set_parent operation.
 * Keep unknown encodings from silently becoming unrelated fixed-rate roots.
 */
static const char *mt6580_smi_parent(u32 sel)
{
	if (!(sel & 8))
		return "dummy26m";
	switch (sel) {
	case 9: return "mt6580_mpll_d5";
	case 10: return "mt6580_mpll_d7";
	case 12: return "mt6580_mpll_d14";
	default: return NULL;
	}
}

static const char *mt6580_usb_parent(u32 sel)
{
	switch (sel) {
	case 1: return "dummy26m";
	case 2: return "mt6580_upll_d16";
	case 4: return "mt6580_mpll_d20";
	default: return NULL;
	}
}

static const char *mt6580_axi_parent(u32 sel)
{
	switch (sel) {
	case 1: return "dummy26m";
	case 2: return "mt6580_mpll_d11";
	case 4: return "mt6580_mpll_d12";
	/* MEMPLL (0xc) needs a DDR clock provider before it can be modeled. */
	default: return NULL;
	}
}

static const char *mt6580_mfg_parent(u32 sel0, u32 sel1)
{
	switch (sel1 & 0x38) {
	case 0x08:
		/* The alternate input is selected by the non-glitch-free mux. */
		if ((sel0 & 5) == 4)
			return "dummy26m";
		return (sel0 & 7) == 2 ? "mt6580_upll_d3" : NULL;
	case 0x10: return "mt6580_mpll_d3";
	case 0x20:
		switch (sel1 & 7) {
		case 1: return "mt6580_mpll_d5";
		case 2: return "mt6580_mpll_d7";
		case 4: return "mt6580_mpll_d14";
		}
		fallthrough;
	default: return NULL;
	}
}

static int mt6580_register_parent_alias(struct device *dev, const char *name,
				       const char *parent, unsigned long flags)
{
	struct clk_hw *hw;

	if (!parent)
		return dev_err_probe(dev, -EINVAL, "Unknown firmware parent for %s\n", name);
	hw = devm_clk_hw_register_fixed_factor(dev, name, parent, flags, 1, 1);
	return PTR_ERR_OR_ZERO(hw);
}

static int mt6580_register_top_sources(struct device *dev, void __iomem *base,
				      struct clk_hw_onecell_data *data)
{
	struct clk_hw *hw;
	u32 sel0 = readl(base), sel1 = readl(base + 4);
	const char *smi = mt6580_smi_parent((sel1 >> 16) & 0xf);
	const char *usb = mt6580_usb_parent((sel1 >> 20) & 7);
	const char *mfg = mt6580_mfg_parent(sel0 >> 8, (sel1 >> 8) & 0x3f);
	const char *axi = mt6580_axi_parent((sel0 >> 4) & 0xf);
	const char *nfi_d5 = NULL;
	unsigned int i;
	int ret;

	hw = devm_clk_hw_register_fixed_rate(dev, "mt6580_mpll", "dummy26m", 0,
					    1501500000);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	hw = devm_clk_hw_register_fixed_rate(dev, "mt6580_upll", "dummy26m", 0,
					    1248000000);
	if (IS_ERR(hw))
		return PTR_ERR(hw);
	for (i = 0; i < ARRAY_SIZE(mt6580_fw_factors); i++) {
		hw = devm_clk_hw_register_fixed_factor(dev, mt6580_fw_factors[i].name,
			mt6580_fw_factors[i].parent, 0, 1, mt6580_fw_factors[i].div);
		if (IS_ERR(hw))
			return PTR_ERR(hw);
	}
	data->hws[CLK_MT6580_TOP_MPLL_D5] = mt6580_register_d5(dev, base);
	if (IS_ERR(data->hws[CLK_MT6580_TOP_MPLL_D5]))
		return PTR_ERR(data->hws[CLK_MT6580_TOP_MPLL_D5]);

	/* CPU and unported infrastructure also use AXI: retain its source. */
	ret = mt6580_register_parent_alias(dev, "mt6580_axi", axi, CLK_IS_CRITICAL);
	if (ret)
		return ret;
	ret = mt6580_register_parent_alias(dev, "mt6580_i2c_src", "mt6580_axi", 0);
	if (ret)
		return ret;
	ret = mt6580_register_parent_alias(dev, "mt6580_smi_src", smi, 0);
	if (ret)
		return ret;
	ret = mt6580_register_parent_alias(dev, "mt6580_usb_bus_src", usb, 0);
	if (ret)
		return ret;
	ret = mt6580_register_parent_alias(dev, "mt6580_mfg_src", mfg, 0);
	if (ret)
		return ret;
	ret = mt6580_register_parent_alias(dev, "mt6580_msdc0_src",
					  mt6580_msdc_parents[(sel0 >> 11) & 7], 0);
	if (ret)
		return ret;
	ret = mt6580_register_parent_alias(dev, "mt6580_disp_pwm_src",
					  sel0 & BIT(18) ? "mt6580_upll_d12" : "dummy26m", 0);
	if (ret)
		return ret;

	/* Unported firmware consumers must not lose D5 when DISP is later
	 * allowed to power off. Retain only their selected D5 branches, not
	 * a blanket critical D5. Drop these holds when their drivers own PM.
	 */
	if (((sel0 >> 20) & 7) == 1) {
		ret = mt6580_register_parent_alias(dev, "mt6580_fw_msdc1",
						  "mt6580_mpll_d10", CLK_IS_CRITICAL);
		if (ret)
			return ret;
	}
	if ((sel1 & 0x7f) == 0x12)
		nfi_d5 = "mt6580_mpll_d10";
	else if ((sel1 & 0x7f) == 0x24)
		nfi_d5 = "mt6580_mpll_d40";
	if (nfi_d5) {
		ret = mt6580_register_parent_alias(dev, "mt6580_fw_nfi",
						  nfi_d5, CLK_IS_CRITICAL);
		if (ret)
			return ret;
	}
	dev_info(dev, "Firmware parents: SMI=%s USB=%s MFG=%s MSDC0=%s; mux=%#x/%#x\n",
		 smi, usb, mfg, mt6580_msdc_parents[(sel0 >> 11) & 7], sel0, sel1);
	dev_info(dev, "AXI parent=%s; I2C/APDMA follow AXI, AUXADC uses 26 MHz\n", axi);
	return 0;
}

static int mt6580_usb_clk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct clk_hw_onecell_data *data;
	struct clk_hw *source;
	void __iomem *base;
	bool top = of_device_is_compatible(dev->of_node, "mediatek,mt6580-topckgen");
	unsigned int count = 3;
	unsigned int i;
	int ret;

	if (of_device_is_compatible(dev->of_node, "mediatek,mt6580-mfgcfg"))
		return mtk_clk_simple_probe(pdev);
	if (top)
		count = 12;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);
	data = devm_kzalloc(dev, struct_size(data, hws, count), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->num = count;

	if (top) {
		ret = mt6580_register_top_sources(dev, base, data);
		if (ret)
			return ret;

		/* SMI clock must remain enabled while the DISP domain is on. */
		data->hws[CLK_MT6580_TOP_SMI] = devm_clk_hw_register_gate(dev,
			"mt6580_smi", "mt6580_smi_src", 0, base + 0x20, 9,
			CLK_GATE_SET_TO_DISABLE, &mt6580_usb_clk_lock);
		data->hws[CLK_MT6580_TOP_THERM] = devm_clk_hw_register_gate(dev,
			"mt6580_therm", "dummy26m", 0, base + 0x24, 1,
			CLK_GATE_SET_TO_DISABLE, &mt6580_usb_clk_lock);
		data->hws[CLK_MT6580_TOP_AUXADC] = devm_clk_hw_register_gate(dev,
			"mt6580_auxadc", "dummy26m", 0, base + 0x24, 30,
			CLK_GATE_SET_TO_DISABLE, &mt6580_usb_clk_lock);
		data->hws[CLK_MT6580_TOP_MSDC0] = devm_clk_hw_register_gate(dev,
			"mt6580_msdc0", "mt6580_msdc0_src", 0, base + 0x20, 17,
			CLK_GATE_SET_TO_DISABLE, &mt6580_usb_clk_lock);

		/* CLK_GATING_CTRL1[13]: 1 gates the USB bus clock. */
		data->hws[CLK_MT6580_TOP_USB] = devm_clk_hw_register_gate(dev,
			"mt6580_usb_bus", "mt6580_usb_bus_src", 0, base + 0x24, 13,
			CLK_GATE_SET_TO_DISABLE, &mt6580_usb_clk_lock);

		/* AXI is divided by ten in the I2C controller. */
		data->hws[CLK_MT6580_TOP_I2C1] = devm_clk_hw_register_gate(dev,
			"mt6580_i2c1", "mt6580_i2c_src", 0, base + 0x24, 4,
			CLK_GATE_SET_TO_DISABLE, &mt6580_usb_clk_lock);
		data->hws[CLK_MT6580_TOP_I2C0] = devm_clk_hw_register_gate(dev,
			"mt6580_i2c0", "mt6580_i2c_src", 0, base + 0x24, 3,
			CLK_GATE_SET_TO_DISABLE, &mt6580_usb_clk_lock);
		data->hws[CLK_MT6580_TOP_I2C2] = devm_clk_hw_register_gate(dev,
			"mt6580_i2c2", "mt6580_i2c_src", 0, base + 0x24, 16,
			CLK_GATE_SET_TO_DISABLE, &mt6580_usb_clk_lock);
		data->hws[CLK_MT6580_TOP_APDMA] = devm_clk_hw_register_gate(dev,
			"mt6580_apdma", "mt6580_axi", 0, base + 0x24, 2,
			CLK_GATE_SET_TO_DISABLE, &mt6580_usb_clk_lock);
		data->hws[CLK_MT6580_TOP_DISP_PWM] = devm_clk_hw_register_gate(dev,
			"mt6580_disp_pwm", "mt6580_disp_pwm_src", 0, base + 0x20, 0,
			CLK_GATE_SET_TO_DISABLE, &mt6580_usb_clk_lock);

		data->hws[CLK_MT6580_TOP_MFG] = devm_clk_hw_register_gate(dev,
			"mt6580_mfg_bus", "mt6580_mfg_src", 0, base + 0x20, 2,
			CLK_GATE_SET_TO_DISABLE, &mt6580_usb_clk_lock);

	} else {
		data->hws[CLK_MT6580_APMIXED_CPU] = mt6580_register_cpu_clk(pdev, base);
		/* USB retains the firmware's UNIVPLL rate and enable state. */
		if (!(readl(base + 0x140) & BIT(0)))
			return dev_err_probe(dev, -EIO,
					     "UNIVPLL is off; firmware clock setup required\n");
		source = devm_clk_hw_register_fixed_factor(dev, "mt6580_usb_48m_src",
						         "mt6580_upll", 0, 1, 26);
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

MODULE_DESCRIPTION("MT6580 CPU, peripheral and GPU clocks");
MODULE_LICENSE("GPL");
