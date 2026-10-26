// SPDX-License-Identifier: GPL-2.0-only
/* CPU, peripheral and GPU clocks from the MT6580 vendor clock manager. */
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
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

/*
 * Expose the firmware-programmed PLLs without taking over tuning or power.
 * MAINPLL has a 7.14 PCW; UNIVPLL has an integer feedback divider. Both
 * have the post-divider in CON1[26:24], as in the BSP PLL register layout.
 */
struct mt6580_pll_clk {
	struct clk_hw hw;
	void __iomem *con;
	bool univ;
};

static unsigned long mt6580_pll_recalc_rate(struct clk_hw *hw,
					 unsigned long parent_rate)
{
	struct mt6580_pll_clk *pll = container_of(hw, struct mt6580_pll_clk, hw);
	u32 con1 = readl(pll->con + 4);
	u64 rate;

	if (pll->univ)
		rate = (u64)parent_rate * (con1 & GENMASK(6, 0));
	else
		rate = ((u64)parent_rate * (con1 & GENMASK(20, 0))) >> 14;
	return rate >> ((con1 >> 24) & 7);
}

static int mt6580_pll_is_enabled(struct clk_hw *hw)
{
	struct mt6580_pll_clk *pll = container_of(hw, struct mt6580_pll_clk, hw);

	return !!(readl(pll->con) & BIT(0));
}

static const struct clk_ops mt6580_pll_ops = {
	.recalc_rate = mt6580_pll_recalc_rate,
	.is_enabled = mt6580_pll_is_enabled,
};

static struct clk_hw *mt6580_register_pll(struct device *dev, void __iomem *con,
					const char *name, bool univ)
{
	const char *parent = "dummy26m";
	struct clk_init_data init = {
		.name = name, .ops = &mt6580_pll_ops,
		.parent_names = &parent, .num_parents = 1,
		.flags = CLK_GET_RATE_NOCACHE,
	};
	struct mt6580_pll_clk *pll;
	int ret;

	if (!(readl(con) & BIT(0)))
		return ERR_PTR(dev_err_probe(dev, -EIO,
					    "%s is off; firmware PLL setup required\n", name));
	pll = devm_kzalloc(dev, sizeof(*pll), GFP_KERNEL);
	if (!pll)
		return ERR_PTR(-ENOMEM);
	pll->con = con;
	pll->univ = univ;
	pll->hw.init = &init;
	ret = devm_clk_hw_register(dev, &pll->hw);
	return ret ? ERR_PTR(ret) : &pll->hw;
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

/* Each PLL divider has its own active-high FREDIV_EN bit. Keep unowned
 * branches read-only until all of their firmware consumers are modeled.
 */
struct mt6580_div_desc {
	const char *name;
	const char *parent;
	u8 div;
	u8 sta;
	u8 shift;
	bool managed;
};

struct mt6580_div_clk {
	struct clk_hw hw;
	void __iomem *reg;
	const struct mt6580_div_desc *desc;
};

static int mt6580_div_enable(struct clk_hw *hw)
{
	struct mt6580_div_clk *div = container_of(hw, struct mt6580_div_clk, hw);

	writel(BIT(div->desc->shift), div->reg + 0x30); /* SET_FREDIV_EN */
	readl(div->reg);
	return 0;
}

static void mt6580_div_disable(struct clk_hw *hw)
{
	struct mt6580_div_clk *div = container_of(hw, struct mt6580_div_clk, hw);

	writel(BIT(div->desc->shift), div->reg + 0x60); /* CLR_FREDIV_EN */
	readl(div->reg);
}

static int mt6580_div_is_enabled(struct clk_hw *hw)
{
	struct mt6580_div_clk *div = container_of(hw, struct mt6580_div_clk, hw);

	return !!(readl(div->reg) & BIT(div->desc->shift));
}

static unsigned long mt6580_div_recalc_rate(struct clk_hw *hw,
					 unsigned long parent_rate)
{
	struct mt6580_div_clk *div = container_of(hw, struct mt6580_div_clk, hw);

	return parent_rate / div->desc->div;
}

static const struct clk_ops mt6580_div_ops = {
	.enable = mt6580_div_enable,
	.disable = mt6580_div_disable,
	.is_enabled = mt6580_div_is_enabled,
	.recalc_rate = mt6580_div_recalc_rate,
};

static const struct clk_ops mt6580_div_ro_ops = {
	.is_enabled = mt6580_div_is_enabled,
	.recalc_rate = mt6580_div_recalc_rate,
};

static struct clk_hw *mt6580_register_div(struct device *dev, void __iomem *base,
					const struct mt6580_div_desc *desc)
{
	struct clk_init_data init = {
		.name = desc->name,
		.ops = desc->managed ? &mt6580_div_ops : &mt6580_div_ro_ops,
		.parent_names = &desc->parent,
		.num_parents = 1,
		.flags = CLK_GET_RATE_NOCACHE,
	};
	struct mt6580_div_clk *div;
	int ret;

	div = devm_kzalloc(dev, sizeof(*div), GFP_KERNEL);
	if (!div)
		return ERR_PTR(-ENOMEM);
	div->reg = base + desc->sta;
	div->desc = desc;
	div->hw.init = &init;
	ret = devm_clk_hw_register(dev, &div->hw);
	return ret ? ERR_PTR(ret) : &div->hw;
}

static const struct mt6580_div_desc mt6580_d5 = {
	"mt6580_mpll_d5", "mt6580_mpll_d5_src", 1, 0x30, 2, true,
};

/* Divider users have Linux references or the explicit firmware holds below.
 * Keep the PLLs and the separate MEMPLL_D2 bit under firmware ownership.
 */
static const struct mt6580_div_desc mt6580_fw_divs[] = {
	{ "mt6580_mpll_d2", "mt6580_mpll", 2, 0x30, 0, true },
	{ "mt6580_mpll_d3", "mt6580_mpll", 3, 0x30, 1, true },
	{ "mt6580_mpll_d5_src", "mt6580_mpll", 5 },
	{ "mt6580_mpll_d7", "mt6580_mpll", 7, 0x30, 3, true },
	{ "mt6580_mpll_d11", "mt6580_mpll", 11, 0x30, 4, true },
	{ "mt6580_mpll_d4", "mt6580_mpll_d2", 2, 0x30, 8, true },
	{ "mt6580_mpll_d6", "mt6580_mpll_d3", 2, 0x30, 9, true },
	{ "mt6580_mpll_d10", "mt6580_mpll_d5", 2, 0x30, 10, true },
	{ "mt6580_mpll_d14", "mt6580_mpll_d7", 2, 0x30, 11, true },
	{ "mt6580_mpll_d8", "mt6580_mpll_d4", 2, 0x30, 16, true },
	{ "mt6580_mpll_d12", "mt6580_mpll_d6", 2, 0x30, 17, true },
	{ "mt6580_mpll_d20", "mt6580_mpll_d10", 2, 0x30, 18, true },
	{ "mt6580_mpll_d28", "mt6580_mpll_d14", 2, 0x30, 19, true },
	{ "mt6580_mpll_d16", "mt6580_mpll_d8", 2, 0x30, 23, true },
	{ "mt6580_mpll_d24", "mt6580_mpll_d12", 2, 0x30, 24, true },
	{ "mt6580_mpll_d40", "mt6580_mpll_d20", 2, 0x30, 25, true },
	{ "mt6580_upll_d2", "mt6580_upll", 2, 0x34, 0, true },
	{ "mt6580_upll_d3", "mt6580_upll", 3, 0x34, 1, true },
	{ "mt6580_upll_d4", "mt6580_upll_d2", 2, 0x34, 8, true },
	{ "mt6580_upll_d5", "mt6580_upll", 5, 0x34, 2, true },
	{ "mt6580_upll_d6", "mt6580_upll_d3", 2, 0x34, 9, true },
	{ "mt6580_upll_d7", "mt6580_upll", 7, 0x34, 3, true },
	{ "mt6580_upll_d8", "mt6580_upll_d4", 2, 0x34, 16, true },
	{ "mt6580_upll_d10", "mt6580_upll_d5", 2, 0x34, 10, true },
	{ "mt6580_upll_d12", "mt6580_upll_d6", 2, 0x34, 17, true },
	{ "mt6580_upll_d16", "mt6580_upll_d8", 2, 0x34, 24, true },
	{ "mt6580_upll_d20", "mt6580_upll_d10", 2, 0x34, 18, true },
	{ "mt6580_upll_d24", "mt6580_upll_d12", 2, 0x34, 25, true },
};

static const char * const mt6580_pmic_spi_parents[] = {
	"mt6580_upll_d20", "mt6580_usb48m", "mt6580_upll_d16", "dummy26m",
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
	case 12: return "mt6580_emi_memory_src";
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

static const char *mt6580_cam_parent(u32 sel)
{
	switch (sel) {
	case 1: return "dummy26m";
	case 2: return "mt6580_usb48m";
	case 4: return "mt6580_upll_d6";
	default: return NULL;
	}
}

static const char *mt6580_nfi_parent(u32 sel)
{
	if ((sel & 0x78) == 8)
		return "dummy26m";
	switch (sel) {
	case 0x11: case 0x52: return "mt6580_mpll_d8";
	case 0x12: return "mt6580_mpll_d10";
	case 0x14: case 0x61: return "mt6580_mpll_d14";
	case 0x21: return "mt6580_mpll_d16";
	case 0x22: return "mt6580_mpll_d28";
	case 0x24: return "mt6580_mpll_d40";
	case 0x62: return "mt6580_mpll_d24";
	case 0x51: return "mt6580_mpll_d7";
	case 0x54: return "mt6580_mpll_d12";
	default: return NULL;
	}
}

static const char *mt6580_audio_parent(u32 sel)
{
	switch (sel) {
	case 1: return "dummy26m";
	case 2: return "mt6580_mpll_d24";
	case 4: return "mt6580_mpll_d12";
	default: return NULL;
	}
}

static const char *mt6580_scam_parent(u32 sel)
{
	switch (sel) {
	case 1: return "dummy26m";
	case 2: return "mt6580_mpll_d14";
	case 4: return "mt6580_mpll_d12";
	default: return NULL;
	}
}

/* These leaves have no Linux driver yet. Do not change their gate or mux;
 * retain the selected parent only when firmware left the leaf enabled.
 * Replace each hold with the consumer's clock reference when it is ported.
 */
static int mt6580_hold_firmware_parent(struct device *dev, void __iomem *reg,
				       u8 bit, const char *name, const char *parent)
{
	if (readl(reg) & BIT(bit))
		return 0;

	dev_info(dev, "Retaining firmware clock %s: parent=%s\n",
		 name, parent ?: "unknown");
	return mt6580_register_parent_alias(dev, name, parent, CLK_IS_CRITICAL);
}

/* The non-glitch-free input may only change while the GF mux bypasses it.
 * CCF enables the new source before reparenting and releases the old one
 * afterwards, matching the BSP's glitch_free_clkmux_sel_op ordering.
 */
struct mt6580_mfg_mux {
	struct clk_hw hw;
	void __iomem *base;
	bool nongf;
};

static const char * const mt6580_mfg_ng_parents[] = {
	"dummy26m", "mt6580_upll_d3",
};

static const char * const mt6580_mfg_gf_parents[] = {
	"mt6580_mfg_ng_src", "mt6580_mpll_d3", "mt6580_mpll_d5",
	"mt6580_mpll_d7", "mt6580_mpll_d14",
};

static const u8 mt6580_mfg_gf_values[] = { 0x08, 0x10, 0x21, 0x22, 0x24 };

static u8 mt6580_mfg_get_parent(struct clk_hw *hw)
{
	struct mt6580_mfg_mux *mux = container_of(hw, struct mt6580_mfg_mux, hw);
	u32 val = readl(mux->base + (mux->nongf ? 0 : 4)) >> 8;
	unsigned int i;

	if (mux->nongf)
		return (val & 5) == 4 ? 0 : 1;
	if ((val & 0x38) == 0x08)
		return 0;
	if ((val & 0x38) == 0x10)
		return 1;
	for (i = 2; i < ARRAY_SIZE(mt6580_mfg_gf_values); i++)
		if ((val & 0x3f) == mt6580_mfg_gf_values[i])
			return i;
	return 0xff;
}

static int mt6580_mfg_set_parent(struct clk_hw *hw, u8 index)
{
	struct mt6580_mfg_mux *mux = container_of(hw, struct mt6580_mfg_mux, hw);
	unsigned long flags;
	void __iomem *reg = mux->base + (mux->nongf ? 0 : 4);
	u32 val, mask = mux->nongf ? 7 : 0x3f;

	if (index >= (mux->nongf ? ARRAY_SIZE(mt6580_mfg_ng_parents) :
					 ARRAY_SIZE(mt6580_mfg_gf_parents)))
		return -EINVAL;
	spin_lock_irqsave(&mt6580_usb_clk_lock, flags);
	if (mux->nongf && ((readl(mux->base + 4) >> 8) & 0x38) == 0x08) {
		spin_unlock_irqrestore(&mt6580_usb_clk_lock, flags);
		return -EBUSY;
	}
	val = readl(reg) & ~(mask << 8);
	val |= (mux->nongf ? (index ? 2 : 4) : mt6580_mfg_gf_values[index]) << 8;
	writel(val, reg);
	readl(reg);
	spin_unlock_irqrestore(&mt6580_usb_clk_lock, flags);
	return 0;
}

static const struct clk_ops mt6580_mfg_mux_ops = {
	.get_parent = mt6580_mfg_get_parent,
	.set_parent = mt6580_mfg_set_parent,
	.determine_rate = __clk_mux_determine_rate,
};

static int mt6580_register_mfg_muxes(struct device *dev, void __iomem *base)
{
	struct mt6580_mfg_mux *mux;
	struct clk_init_data init = {
		.ops = &mt6580_mfg_mux_ops,
		.flags = CLK_SET_RATE_PARENT,
	};
	u32 ng = (readl(base) >> 8) & 7;
	int i, ret;

	if ((ng & 5) != 4 && ng != 2)
		return dev_err_probe(dev, -EINVAL, "Unknown firmware MFG non-GF mux\n");
	for (i = 0; i < 2; i++) {
		mux = devm_kzalloc(dev, sizeof(*mux), GFP_KERNEL);
		if (!mux)
			return -ENOMEM;
		mux->base = base;
		mux->nongf = !i;
		init.name = i ? "mt6580_mfg_src" : "mt6580_mfg_ng_src";
		init.parent_names = i ? mt6580_mfg_gf_parents : mt6580_mfg_ng_parents;
		init.num_parents = i ? ARRAY_SIZE(mt6580_mfg_gf_parents) :
				      ARRAY_SIZE(mt6580_mfg_ng_parents);
		init.flags = CLK_SET_RATE_PARENT | (i ? 0 : CLK_SET_PARENT_GATE);
		mux->hw.init = &init;
		ret = devm_clk_hw_register(dev, &mux->hw);
		if (ret)
			return ret;
	}
	return 0;
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
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(mt6580_fw_divs); i++) {
		const struct mt6580_div_desc *desc = &mt6580_fw_divs[i];

		if (desc->sta)
			hw = mt6580_register_div(dev, base, desc);
		else
			hw = devm_clk_hw_register_fixed_factor(dev, desc->name,
							      desc->parent, 0, 1, desc->div);
		if (IS_ERR(hw))
			return PTR_ERR(hw);
	}
	data->hws[CLK_MT6580_TOP_MPLL_D5] = mt6580_register_div(dev, base, &mt6580_d5);
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
	if (!mfg)
		return dev_err_probe(dev, -EINVAL, "Unknown firmware MFG mux\n");
	ret = mt6580_register_mfg_muxes(dev, base);
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
	ret = mt6580_register_parent_alias(dev, "mt6580_pmic_spi_src",
					  mt6580_pmic_spi_parents[(sel0 >> 24) & 3], 0);
	if (ret)
		return ret;
	ret = mt6580_register_parent_alias(dev, "mt6580_uart0_src",
					  sel0 & BIT(0) ? "mt6580_upll_d24" : "dummy26m", 0);
	if (ret)
		return ret;
	ret = mt6580_register_parent_alias(dev, "mt6580_uart1_src",
					  sel0 & BIT(19) ? "mt6580_upll_d24" : "dummy26m", 0);
	if (ret)
		return ret;

	ret = mt6580_hold_firmware_parent(dev, base + 0x24, 18, "mt6580_fw_msdc1",
					  mt6580_msdc_parents[(sel0 >> 20) & 7]);
	if (ret)
		return ret;
	ret = mt6580_hold_firmware_parent(dev, base + 0x24, 23, "mt6580_fw_spi",
					  sel1 & BIT(14) ? "mt6580_upll_d12" : "dummy26m");
	if (ret)
		return ret;
	ret = mt6580_hold_firmware_parent(dev, base + 0x20, 1, "mt6580_fw_cam",
					  mt6580_cam_parent((sel0 >> 15) & 7));
	if (ret)
		return ret;

	ret = mt6580_hold_firmware_parent(dev, base + 0x24, 19, "mt6580_fw_nfi",
					  mt6580_nfi_parent(sel1 & 0x7f));
	if (ret)
		return ret;
	ret = mt6580_hold_firmware_parent(dev, base + 0x24, 25, "mt6580_fw_audio",
					  mt6580_audio_parent((sel0 >> 27) & 7));
	if (ret)
		return ret;
	ret = mt6580_hold_firmware_parent(dev, base + 0x20, 5, "mt6580_fw_scam",
					  mt6580_scam_parent((sel1 >> 23) & 7));
	if (ret)
		return ret;

	/* The BSP explicitly exempts D14 from unused-divider cleanup. Keep
	 * that policy without inventing a DSI gate parent absent from the BSP.
	 */
	ret = mt6580_register_parent_alias(dev, "mt6580_fw_d14",
					  "mt6580_mpll_d14", CLK_IS_CRITICAL);
	if (ret)
		return ret;
	/* Atomic ARMPLL transitions temporarily use PLL2_CK from MPLL_D2. */
	ret = mt6580_register_parent_alias(dev, "mt6580_cpu_handover",
					  "mt6580_mpll_d2", CLK_IS_CRITICAL);
	if (ret)
		return ret;

	/* DDRPHY configuration access and live EMI have no runtime gate owner.
	 * Retain their boot muxes; do not modify memory PLLs or bit 31 in 0x30.
	 */
	data->hws[CLK_MT6580_TOP_DDRPHYCFG] = devm_clk_hw_register_fixed_factor(dev,
		"mt6580_ddrphycfg", sel1 & BIT(15) ? "mt6580_mpll_d16" : "dummy26m",
		CLK_IS_CRITICAL, 1, 1);
	if (IS_ERR(data->hws[CLK_MT6580_TOP_DDRPHYCFG]))
		return PTR_ERR(data->hws[CLK_MT6580_TOP_DDRPHYCFG]);
	data->hws[CLK_MT6580_TOP_EMI] = devm_clk_hw_register_fixed_factor(dev,
		"mt6580_emi", sel0 & BIT(1) ? "mt6580_emi_memory_src" : "dummy26m",
		CLK_IS_CRITICAL, 1, 1);
	if (IS_ERR(data->hws[CLK_MT6580_TOP_EMI]))
		return PTR_ERR(data->hws[CLK_MT6580_TOP_EMI]);
	dev_info(dev, "Firmware parents: SMI=%s USB=%s MFG=%s MSDC0=%s; mux=%#x/%#x\n",
		 smi, usb, mfg, mt6580_msdc_parents[(sel0 >> 11) & 7], sel0, sel1);
	dev_info(dev, "AXI parent=%s; I2C/APDMA follow AXI, AUXADC uses 26 MHz\n", axi);
	return 0;
}

static unsigned long mt6580_uart_clk_flags(void __iomem *base, unsigned int id)
{
	struct device_node *np;
	struct resource res;
	unsigned long flags = 0;

	/* The console may write with interrupts off and cannot runtime-resume
	 * a clock. Preserve the selected stdout UART, including panic output.
	 */
	if (IS_ENABLED(CONFIG_SERIAL_8250_CONSOLE) && of_stdout &&
	    !of_address_to_resource(of_stdout, 0, &res) &&
	    res.start == 0x11005000 + id * 0x1000)
		return CLK_IS_CRITICAL;

	/* Retain an enabled firmware UART without an active Linux consumer. */
	np = of_find_node_by_path(id ? "serial1" : "serial0");
	if (!of_device_is_available(np) && !(readl(base + 0x24) & BIT(10 + id)))
		flags = CLK_IS_CRITICAL;
	of_node_put(np);
	return flags;
}

static int mt6580_usb_clk_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct clk_hw_onecell_data *data;
	struct clk_hw *source;
	void __iomem *base;
	bool top = of_device_is_compatible(dev->of_node, "mediatek,mt6580-topckgen");
	unsigned int count = 5;
	unsigned int i;
	int ret;

	if (of_device_is_compatible(dev->of_node, "mediatek,mt6580-mfgcfg"))
		return mtk_clk_simple_probe(pdev);
	if (top)
		count = 18;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);
	data = devm_kzalloc(dev, struct_size(data, hws, count), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->num = count;

	if (top) {
		struct clk *parent;

		/* Consumers may use gates as soon as this provider is published.
		 * Wait for both real roots, rather than exposing zero-rate orphans.
		 */
		parent = devm_clk_get(dev, "mainpll");
		if (IS_ERR(parent))
			return dev_err_probe(dev, PTR_ERR(parent), "MAINPLL unavailable\n");
		parent = devm_clk_get(dev, "univpll");
		if (IS_ERR(parent))
			return dev_err_probe(dev, PTR_ERR(parent), "UNIVPLL unavailable\n");
		parent = devm_clk_get(dev, "memory");
		if (IS_ERR(parent))
			return dev_err_probe(dev, PTR_ERR(parent), "Memory clock unavailable\n");
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
			"mt6580_msdc0", "mt6580_msdc0_src", 0, base + 0x24, 17,
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
			"mt6580_mfg_bus", "mt6580_mfg_src", CLK_SET_RATE_PARENT, base + 0x20, 2,
			CLK_GATE_SET_TO_DISABLE, &mt6580_usb_clk_lock);
		data->hws[CLK_MT6580_TOP_PMIC_SPI] = devm_clk_hw_register_gate(dev,
			"mt6580_pmic_spi", "mt6580_pmic_spi_src", 0, base + 0x24, 20,
			CLK_GATE_SET_TO_DISABLE, &mt6580_usb_clk_lock);
		data->hws[CLK_MT6580_TOP_PMIC_26M] = devm_clk_hw_register_gate(dev,
			"mt6580_pmic_26m", "dummy26m", 0, base + 0x24, 29,
			CLK_GATE_SET_TO_DISABLE, &mt6580_usb_clk_lock);
		data->hws[CLK_MT6580_TOP_UART0] = devm_clk_hw_register_gate(dev,
			"mt6580_uart0", "mt6580_uart0_src", mt6580_uart_clk_flags(base, 0),
			base + 0x24, 10, CLK_GATE_SET_TO_DISABLE, &mt6580_usb_clk_lock);
		data->hws[CLK_MT6580_TOP_UART1] = devm_clk_hw_register_gate(dev,
			"mt6580_uart1", "mt6580_uart1_src", mt6580_uart_clk_flags(base, 1),
			base + 0x24, 11, CLK_GATE_SET_TO_DISABLE, &mt6580_usb_clk_lock);

	} else {
		data->hws[CLK_MT6580_APMIXED_MAINPLL] =
			mt6580_register_pll(dev, base + 0x120, "mt6580_mpll", false);
		if (IS_ERR(data->hws[CLK_MT6580_APMIXED_MAINPLL]))
			return PTR_ERR(data->hws[CLK_MT6580_APMIXED_MAINPLL]);
		data->hws[CLK_MT6580_APMIXED_UNIVPLL] =
			mt6580_register_pll(dev, base + 0x140, "mt6580_upll", true);
		if (IS_ERR(data->hws[CLK_MT6580_APMIXED_UNIVPLL]))
			return PTR_ERR(data->hws[CLK_MT6580_APMIXED_UNIVPLL]);
		data->hws[CLK_MT6580_APMIXED_CPU] = mt6580_register_cpu_clk(pdev, base);
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
