// SPDX-License-Identifier: GPL-2.0-only
/* CPU, peripheral and GPU clocks from the MT6580 vendor clock manager. */
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/irqflags.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/smp.h>
#include <linux/soc/mediatek/mt6580-mainpll.h>
#include <linux/soc/mediatek/mt6580-pwrap.h>

#include <asm/barrier.h>
#include "clk-gate.h"
#include "clk-mtk.h"

#include <dt-bindings/clock/mt6580-clk.h>

static DEFINE_SPINLOCK(mt6580_usb_clk_lock);

struct mt6580_clk_provider {
	void __iomem *base;
	void __iomem *infra;
	struct clk_hw_onecell_data *hws;
	bool top, apmixed;
	bool dcm_owned;
	u32 dcm_before[6];
	u32 dcm_applies, dcm_resume_fixes;
};

static struct platform_driver mt6580_usb_clk_driver;
/* MFGCFG shares this driver but uses mtk_clk_simple's private data. Only
 * these probe-validated bank identities may expose mt6580_clk_provider.
 */
static struct device *mt6580_top_device;
static struct device *mt6580_pll_device;

/* CPU Mode 1 and infra/peri policy from MT6580 mt_dcm.c and the normal
 * kernel backup. The six fields span existing TOPCKGEN/INFRACFG owners.
 * Do not own CPU/L2 mux/dividers, MCUSYS, EMI/DRAMC, PMIC or USB DCM.
 */
static const u16 mt6580_dcm_offsets[] = { 0x14, 0x10, 0xa00, 0x20, 0x28, 0x2c };
static const u32 mt6580_dcm_masks[] = {
	BIT(0), GENMASK(2, 1), BIT(2), BIT(30),
	GENMASK(11, 5) | GENMASK(27, 21), GENMASK(19, 8),
};
static const u32 mt6580_dcm_on[] = {
	BIT(0), 0, BIT(2), 0, BIT(11) | BIT(10) | BIT(27) | BIT(26),
	(7 << 9) | BIT(8) | (7 << 15) | BIT(14),
};
static const u8 mt6580_dcm_enable_order[] = { 5, 4, 2, 1, 3, 0 };
static const u8 mt6580_dcm_pause_order[] = { 0, 1, 2, 3, 4, 5 };

static void __iomem *mt6580_dcm_reg(void __iomem *top, void __iomem *infra, unsigned int i)
{
	return (i < 3 ? infra : top) + mt6580_dcm_offsets[i];
}

static void mt6580_dcm_snapshot(void __iomem *top, void __iomem *infra, u32 values[6])
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mt6580_dcm_offsets); i++)
		values[i] = readl(mt6580_dcm_reg(top, infra, i));
}

static bool mt6580_dcm_matches(const u32 values[6], const u32 expected[6])
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mt6580_dcm_offsets); i++)
		if ((values[i] ^ expected[i]) & mt6580_dcm_masks[i])
			return false;
	return true;
}

/* Caller owns the shared clock lock, or is the sole CPU with IRQs disabled
 * in the coherency-off finisher. No locking, allocation or CCF in this helper.
 */
static int mt6580_dcm_write(void __iomem *top, void __iomem *infra,
			   const u32 values[6], const u8 order[6])
{
	unsigned int n;

	for (n = 0; n < ARRAY_SIZE(mt6580_dcm_offsets); n++) {
		unsigned int i = order[n];
		void __iomem *reg = mt6580_dcm_reg(top, infra, i);
		u32 mask = mt6580_dcm_masks[i];
		u32 value = (readl(reg) & ~mask) | (values[i] & mask);

		writel(value, reg);
		if ((readl(reg) ^ value) & mask)
			return -EIO;
	}
	return 0;
}

static void mt6580_dcm_put_supplier(void *data)
{
	put_device(data);
}

static int mt6580_dcm_init(struct device *dev, struct mt6580_clk_provider *top)
{
	struct device *pll = READ_ONCE(mt6580_pll_device);
	struct mt6580_clk_provider *ap;
	unsigned long flags;
	u32 bus, values[6];
	int ret, restore;

	if (!IS_ENABLED(CONFIG_COMMON_CLK_MT6580_DCM))
		return 0;
	if (!pll || !device_is_bound(pll))
		return -EPROBE_DEFER;
	get_device(pll);
	ret = devm_add_action_or_reset(dev, mt6580_dcm_put_supplier, pll);
	if (ret)
		return ret;
	if (!device_link_add(dev, pll, DL_FLAG_AUTOREMOVE_CONSUMER))
		return -EINVAL;
	if (pll->driver != &mt6580_usb_clk_driver.driver)
		return -ENODEV;
	ap = dev_get_drvdata(pll);
	if (!ap || !ap->apmixed || !ap->infra)
		return -ENODEV;
	/* This mapping remains owned by the held APMIXEDSYS supplier. */
	top->infra = ap->infra;
	spin_lock_irqsave(&mt6580_usb_clk_lock, flags);
	mt6580_dcm_snapshot(top->base, top->infra, top->dcm_before);
	bus = top->dcm_before[4];
	/* Qualify the existing full-rate selections and absence of forced
	 * slow/off. Never silently replace a firmware bus ratio or CPU source.
	 */
	if ((readl(top->infra) & GENMASK(3, 2)) != BIT(2) ||
	    (bus & (GENMASK(4, 0) | GENMASK(20, 16))) != (BIT(4) | BIT(20)) ||
	    (bus & (BIT(31) | BIT(29) | BIT(28) | BIT(15) | BIT(13) | BIT(12)))) {
		ret = -EBUSY;
		goto out;
	}
	ret = mt6580_dcm_write(top->base, top->infra, mt6580_dcm_on, mt6580_dcm_enable_order);
	if (ret) {
		restore = mt6580_dcm_write(top->base, top->infra, top->dcm_before,
					  mt6580_dcm_enable_order);
		if (restore)
			dev_err(dev, "DCM rollback failed: %d\n", restore);
		goto out;
	}
	mt6580_dcm_snapshot(top->base, top->infra, values);
	top->dcm_owned = true;
	top->dcm_applies++;
out:
	spin_unlock_irqrestore(&mt6580_usb_clk_lock, flags);
	return ret;
}

static ssize_t dcm_state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mt6580_clk_provider *top = dev_get_drvdata(dev);
	unsigned long flags;
	u32 values[6] = { 0 };
	ssize_t len;

	spin_lock_irqsave(&mt6580_usb_clk_lock, flags);
	if (top->infra)
		mt6580_dcm_snapshot(top->base, top->infra, values);
	len = sysfs_emit(buf, "owned=%u valid=%u applies=%u resume_fixes=%u\n"
		"regs=infra:0014/0010/0a00 top:0020/0028/002c\n"
		"before=%08x/%08x/%08x/%08x/%08x/%08x\n"
		"current=%08x/%08x/%08x/%08x/%08x/%08x\n",
		top->dcm_owned, top->dcm_owned && mt6580_dcm_matches(values, mt6580_dcm_on),
		top->dcm_applies, top->dcm_resume_fixes,
		top->dcm_before[0], top->dcm_before[1], top->dcm_before[2],
		top->dcm_before[3], top->dcm_before[4], top->dcm_before[5],
		values[0], values[1], values[2], values[3], values[4], values[5]);
	spin_unlock_irqrestore(&mt6580_usb_clk_lock, flags);
	return len;
}
static DEVICE_ATTR_RO(dcm_state);

static umode_t mt6580_clk_attr_visible(struct kobject *kobj, struct attribute *attr, int n)
{
	/* MFGCFG uses a different private struct despite sharing this driver. */
	return of_device_is_compatible(kobj_to_dev(kobj)->of_node,
				      "mediatek,mt6580-topckgen") ? attr->mode : 0;
}

static struct attribute *mt6580_clk_attrs[] = { &dev_attr_dcm_state.attr, NULL };
static const struct attribute_group mt6580_clk_group = {
	.attrs = mt6580_clk_attrs,
	.is_visible = mt6580_clk_attr_visible,
};
static const struct attribute_group *mt6580_clk_groups[] = { &mt6580_clk_group, NULL };

static int mt6580_clk_resume_noirq(struct device *dev)
{
	struct mt6580_clk_provider *top;
	unsigned long flags;
	u32 values[6];
	int ret = 0;

	if (!of_device_is_compatible(dev->of_node, "mediatek,mt6580-topckgen"))
		return 0;
	top = dev_get_drvdata(dev);
	if (!top->dcm_owned)
		return 0;
	spin_lock_irqsave(&mt6580_usb_clk_lock, flags);
	mt6580_dcm_snapshot(top->base, top->infra, values);
	if (!mt6580_dcm_matches(values, mt6580_dcm_on)) {
		ret = mt6580_dcm_write(top->base, top->infra, mt6580_dcm_on,
					mt6580_dcm_enable_order);
		if (!ret) {
			top->dcm_applies++;
			top->dcm_resume_fixes++;
		}
	}
	spin_unlock_irqrestore(&mt6580_usb_clk_lock, flags);
	return ret;
}
static DEFINE_NOIRQ_DEV_PM_OPS(mt6580_clk_pm, NULL, mt6580_clk_resume_noirq);

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
	u32 mux, div, gate, con, dcm;
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
	/* Suspend automatic ARMPLL gating while both PLLs are manipulated.
	 * CPU Mode 1 is restored only after the CPU has returned to ARMPLL.
	 */
	dcm = readl(cpu->infra + 0x14);
	writel(dcm & ~BIT(0), cpu->infra + 0x14);
	mux = readl(cpu->infra);
	div = readl(cpu->infra + 8);
	gate = readl(cpu->infra + 0xa00);
	writel(gate & ~(BIT(1) | BIT(2)), cpu->infra + 0xa00);
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
	writel(dcm, cpu->infra + 0x14);
	readl(cpu->infra + 0x14);
	spin_unlock_irqrestore(&mt6580_usb_clk_lock, flags);
	return 0;
}

static const struct clk_ops mt6580_cpu_ops = {
	.recalc_rate = mt6580_cpu_recalc_rate,
	.round_rate = mt6580_cpu_round_rate,
	.set_rate = mt6580_cpu_set_rate,
};

static struct clk_hw *mt6580_register_cpu_clk(struct platform_device *pdev,
					    void __iomem *base, void __iomem *infra)
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
	cpu->infra = infra;
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

/* The PHY PLL powers up in .prepare, so its reference gates must also use
 * prepare callbacks. Ordinary enable callbacks would run after PLL setup
 * and disable the reference before the PLL's unprepare callback.
 */
struct mt6580_mipi26m_clk {
	struct clk_hw hw;
	struct device *dev;
	void __iomem *base;
	bool top;
};

static int mt6580_mipi26m_is_prepared(struct clk_hw *hw)
{
	struct mt6580_mipi26m_clk *gate = container_of(hw, struct mt6580_mipi26m_clk, hw);
	u32 value = readl(gate->base + (gate->top ? 0x20 : 0));

	return !!(value & BIT(gate->top ? 4 : 5));
}

static int mt6580_mipi26m_prepare(struct clk_hw *hw)
{
	struct mt6580_mipi26m_clk *gate = container_of(hw, struct mt6580_mipi26m_clk, hw);
	struct clk_hw *parent = clk_hw_get_parent(hw);
	unsigned long flags;
	u32 value;

	/* APMIXED registers before TOPCKGEN; do not use its temporary orphan. */
	if (!parent || !clk_hw_get_rate(parent))
		return -EPROBE_DEFER;

	spin_lock_irqsave(&mt6580_usb_clk_lock, flags);
	if (gate->top) {
		/* MIPI_26M_DBG_EN is active high, unlike the adjacent gate bits. */
		writel(BIT(4), gate->base + 0x50);
		value = readl(gate->base + 0x20);
	} else {
		value = readl(gate->base);
		writel(value | BIT(5), gate->base);
		value = readl(gate->base);
	}
	spin_unlock_irqrestore(&mt6580_usb_clk_lock, flags);

	if (!(value & BIT(gate->top ? 4 : 5))) {
		dev_err(gate->dev, "%s reference enable did not latch\n", clk_hw_get_name(hw));
		return -EIO;
	}

	return 0;
}

static void mt6580_mipi26m_unprepare(struct clk_hw *hw)
{
	struct mt6580_mipi26m_clk *gate = container_of(hw, struct mt6580_mipi26m_clk, hw);
	unsigned long flags;
	u32 value;

	spin_lock_irqsave(&mt6580_usb_clk_lock, flags);
	if (gate->top) {
		writel(BIT(4), gate->base + 0x80);
		value = readl(gate->base + 0x20);
	} else {
		value = readl(gate->base);
		writel(value & ~BIT(5), gate->base);
		value = readl(gate->base);
	}
	spin_unlock_irqrestore(&mt6580_usb_clk_lock, flags);

	if (value & BIT(gate->top ? 4 : 5))
		dev_err(gate->dev, "%s reference disable did not latch\n", clk_hw_get_name(hw));
}

static const struct clk_ops mt6580_mipi26m_ops = {
	.prepare = mt6580_mipi26m_prepare,
	.unprepare = mt6580_mipi26m_unprepare,
	.is_prepared = mt6580_mipi26m_is_prepared,
	.is_enabled = mt6580_mipi26m_is_prepared,
};

static struct clk_hw *mt6580_register_mipi26m(struct device *dev,
					   void __iomem *base, bool top)
{
	const char *parent = top ? "dummy26m" : "mt6580_mipi26m_top";
	struct clk_init_data init = {
		.name = top ? "mt6580_mipi26m_top" : "mt6580_mipi26m",
		.ops = &mt6580_mipi26m_ops,
		.parent_names = &parent,
		.num_parents = 1,
		/* Preserve LK scanout until the PHY explicitly takes ownership. */
		.flags = CLK_IGNORE_UNUSED,
	};
	struct mt6580_mipi26m_clk *gate;
	int ret;

	gate = devm_kzalloc(dev, sizeof(*gate), GFP_KERNEL);
	if (!gate)
		return ERR_PTR(-ENOMEM);
	gate->dev = dev;
	gate->base = base;
	gate->top = top;
	gate->hw.init = &init;
	ret = devm_clk_hw_register(dev, &gate->hw);
	return ret ? ERR_PTR(ret) : &gate->hw;
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

/* Retain SMI/USB boot muxes decoded below; these aliases cannot reparent.
 * PMIC SPI and MFG use separate real muxes for their qualified operations.
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
	{
		struct clk_hw *spi;

		spi = devm_clk_hw_register_mux(dev, "mt6580_pmic_spi_src",
			mt6580_pmic_spi_parents, ARRAY_SIZE(mt6580_pmic_spi_parents),
			0, base, 24, 2, 0, &mt6580_usb_clk_lock);
		if (IS_ERR(spi))
			return PTR_ERR(spi);
	}
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
	struct mt6580_clk_provider *provider;
	struct clk_hw_onecell_data *data;
	struct clk_hw *source;
	void __iomem *base;
	bool top = of_device_is_compatible(dev->of_node, "mediatek,mt6580-topckgen");
	unsigned int count = 6;
	unsigned int i;
	int ret;

	if (of_device_is_compatible(dev->of_node, "mediatek,mt6580-mfgcfg"))
		return mtk_clk_simple_probe(pdev);
	if (top)
		count = 20;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base))
		return PTR_ERR(base);
	provider = devm_kzalloc(dev, sizeof(*provider), GFP_KERNEL);
	if (!provider)
		return -ENOMEM;
	provider->base = base;
	provider->top = top;
	provider->apmixed = of_device_is_compatible(dev->of_node, "mediatek,mt6580-apmixedsys");
	if (!top) {
		provider->infra = devm_platform_ioremap_resource(pdev, 1);
		if (IS_ERR(provider->infra))
			return PTR_ERR(provider->infra);
	}
	data = devm_kzalloc(dev, struct_size(data, hws, count), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->num = count;
	provider->hws = data;

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
		/* SMP and genpd also access this always-on SPM register clock. */
		data->hws[CLK_MT6580_TOP_SPM] = devm_clk_hw_register_gate(dev,
			"mt6580_spm", "dummy26m", CLK_IS_CRITICAL, base + 0x24, 26,
			CLK_GATE_SET_TO_DISABLE, &mt6580_usb_clk_lock);
		data->hws[CLK_MT6580_TOP_MIPI26M] = mt6580_register_mipi26m(dev, base, true);

	} else {
		data->hws[CLK_MT6580_APMIXED_MAINPLL] =
			mt6580_register_pll(dev, base + 0x120, "mt6580_mpll", false);
		if (IS_ERR(data->hws[CLK_MT6580_APMIXED_MAINPLL]))
			return PTR_ERR(data->hws[CLK_MT6580_APMIXED_MAINPLL]);
		data->hws[CLK_MT6580_APMIXED_UNIVPLL] =
			mt6580_register_pll(dev, base + 0x140, "mt6580_upll", true);
		if (IS_ERR(data->hws[CLK_MT6580_APMIXED_UNIVPLL]))
			return PTR_ERR(data->hws[CLK_MT6580_APMIXED_UNIVPLL]);
		data->hws[CLK_MT6580_APMIXED_CPU] =
			mt6580_register_cpu_clk(pdev, base, provider->infra);
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
		/* Parent preparation opens TOP first; unprepare closes APMIXED first. */
		data->hws[CLK_MT6580_APMIXED_MIPI26M] = mt6580_register_mipi26m(dev, base, false);
	}
	for (i = 0; i < count; i++)
		if (IS_ERR(data->hws[i]))
			return PTR_ERR(data->hws[i]);

	platform_set_drvdata(pdev, provider);
	if (top) {
		ret = mt6580_dcm_init(dev, provider);
		if (ret)
			return dev_err_probe(dev, ret, "DCM configuration failed\n");
	}
	ret = devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, data);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, provider);
	if (provider->top)
		WRITE_ONCE(mt6580_top_device, dev);
	if (provider->apmixed)
		WRITE_ONCE(mt6580_pll_device, dev);
	return 0;
}

#define MT6580_AXI_MUX_MASK	GENMASK(7, 4)
#define MT6580_AXI_26M		BIT(4)
#define MT6580_AXI_MPLL_D12	BIT(6)
#define MT6580_CPU_MUX_MASK	GENMASK(3, 2)
#define MT6580_CPU_ARMPLL		BIT(2)
#define MT6580_MAINPLL_CON0	0x120
#define MT6580_MAINPLL_PWR_CON	0x130
#define MT6580_MAINPLL_EN		BIT(0)
#define MT6580_MAINPLL_SDM	BIT(4)
#define MT6580_MAINPLL_RSTB	BIT(27)
#define MT6580_MAINPLL_PCW_CHG	BIT(31)
#define MT6580_PLL_PWR_ON		BIT(0)
#define MT6580_PLL_ISO_EN		BIT(1)

#define MT6580_PWRAP_CG_MASK	(BIT(20) | BIT(27) | BIT(28) | BIT(29))

static struct mt6580_clk_provider *mt6580_pwrap_top(struct device *top)
{
	struct mt6580_clk_provider *provider;

	if (!top || top != READ_ONCE(mt6580_top_device) ||
	    top->driver != &mt6580_usb_clk_driver.driver)
		return NULL;
	provider = dev_get_drvdata(top);
	return provider && provider->top ? provider : NULL;
}

int mt6580_pwrap_clocks_gate(struct device *top, u32 *enabled)
{
	struct mt6580_clk_provider *provider = mt6580_pwrap_top(top);
	unsigned long flags;
	u32 status;

	if (!provider)
		return -ENODEV;
	spin_lock_irqsave(&mt6580_usb_clk_lock, flags);
	*enabled = ~readl(provider->base + 0x24) & MT6580_PWRAP_CG_MASK;
	writel(MT6580_PWRAP_CG_MASK, provider->base + 0x54);
	status = readl(provider->base + 0x24);
	if ((status & MT6580_PWRAP_CG_MASK) != MT6580_PWRAP_CG_MASK)
		writel(*enabled, provider->base + 0x84);
	spin_unlock_irqrestore(&mt6580_usb_clk_lock, flags);
	return (status & MT6580_PWRAP_CG_MASK) == MT6580_PWRAP_CG_MASK ? 0 : -EIO;
}
EXPORT_SYMBOL_GPL(mt6580_pwrap_clocks_gate);

int mt6580_pwrap_clocks_restore(struct device *top, u32 enabled)
{
	struct mt6580_clk_provider *provider = mt6580_pwrap_top(top);
	unsigned long flags;
	u32 status;

	if (!provider)
		return -ENODEV;
	enabled = (enabled & MT6580_PWRAP_CG_MASK) | BIT(20);
	spin_lock_irqsave(&mt6580_usb_clk_lock, flags);
	writel(enabled, provider->base + 0x84);
	status = readl(provider->base + 0x24);
	/* BSP WHQA_00014186: PMIC bclk DCM must stay disabled. SET only
	 * these two bits; do not replace unrelated global DCM settings.
	 */
	writel(BIT(1) | BIT(2), provider->base + 0x5c);
	spin_unlock_irqrestore(&mt6580_usb_clk_lock, flags);
	return (~status & MT6580_PWRAP_CG_MASK) == enabled ? 0 : -EIO;
}
EXPORT_SYMBOL_GPL(mt6580_pwrap_clocks_restore);

static int mt6580_axi_get_providers(struct device *top, struct device *pll,
				   struct mt6580_clk_provider **top_data,
				   struct mt6580_clk_provider **pll_data)
{
	if (!top || !pll || top->driver != &mt6580_usb_clk_driver.driver ||
	    pll->driver != &mt6580_usb_clk_driver.driver ||
	    top != READ_ONCE(mt6580_top_device) || pll != READ_ONCE(mt6580_pll_device))
		return -ENODEV;
	/* Compatible roles were checked at probe. Do not call OF helpers in
	 * the coherency-off finisher: their devtree lock is not usable there.
	 * The caller owns bound suppliers through device links; only inspect
	 * private data after validating the driver and probe-published identity.
	 */
	*top_data = dev_get_drvdata(top);
	*pll_data = dev_get_drvdata(pll);
	if (!*top_data || !*pll_data || !(*top_data)->top || !(*pll_data)->apmixed ||
	    !(*top_data)->base || !(*top_data)->hws ||
	    !(*pll_data)->base || !(*pll_data)->infra || !(*pll_data)->hws)
		return -ENODEV;
	return 0;
}

static bool mt6580_axi_atomic_context(void)
{
	return irqs_disabled() && num_online_cpus() == 1 && !smp_processor_id();
}

static void mt6580_mainpll_snapshot(struct mt6580_clk_provider *pll, u32 values[5])
{
	unsigned int i;

	/* CON0/CON1/CON2/CON3/PWR_CON0: the power word is +0x130. */
	for (i = 0; i < 5; i++)
		values[i] = readl(pll->base + MT6580_MAINPLL_CON0 + i * 4);
}

static bool mt6580_mainpll_ready(const u32 values[5])
{
	u32 enabled = MT6580_MAINPLL_EN | MT6580_MAINPLL_RSTB;
	u32 power = MT6580_PLL_PWR_ON | MT6580_PLL_ISO_EN;

	/* The BSP defines only these power bits; other bits may be status. */
	return (values[0] & enabled) == enabled &&
		(values[4] & power) == MT6580_PLL_PWR_ON;
}

static bool mt6580_mainpll_matches(const u32 left[5], const u32 right[5])
{
	unsigned int i;

	for (i = 0; i < 5; i++) {
		/* CON1[31] is the BSP's PCW-change strobe, not configuration. */
		u32 mask = i == 1 ? ~MT6580_MAINPLL_PCW_CHG : ~0U;

		if ((left[i] ^ right[i]) & mask)
			return false;
	}
	return true;
}

static bool mt6580_mainpll_config_matches(const u32 values[5], const u32 before[5])
{
	unsigned int i;

	/* Intermediate PWR state has no BSP-defined semantics above bit 1.
	 * Do not compare unknown status against its powered-on baseline here;
	 * the caller checks owned power bits, and final matches stays strict.
	 */
	for (i = 0; i < 4; i++) {
		u32 mask = ~0U;

		if (!i)
			mask &= ~(MT6580_MAINPLL_EN | MT6580_MAINPLL_SDM | MT6580_MAINPLL_RSTB);
		else if (i == 1)
			mask &= ~MT6580_MAINPLL_PCW_CHG;
		if ((values[i] ^ before[i]) & mask)
			return false;
	}
	return true;
}

static int mt6580_axi_error(struct mt6580_axi_context *ctx, int error)
{
	if (!ctx->error)
		ctx->error = error;
	return error;
}

static void mt6580_axi_sync(void)
{
#if defined(CONFIG_ARM) || defined(CONFIG_ARM64)
	/* Keep the finisher independent of outer-cache locking. */
	dsb(sy);
	isb();
#else
	/* This driver can also be selected by COMPILE_TEST. */
	mb();
#endif
}

static u32 mt6580_axi_write_mux(struct mt6580_clk_provider *top, u32 mux)
{
	mt6580_axi_sync();
	writel(mux, top->base);
	mux = readl(top->base);
	mt6580_axi_sync();
	return mux;
}

static u32 mt6580_axi_keep_26m(struct mt6580_clk_provider *top)
{
	u32 mux = readl(top->base);

	/* The BSP's AXI 26 MHz encoding is 1, not the unmapped value 0. */
	return mt6580_axi_write_mux(top, (mux & ~MT6580_AXI_MUX_MASK) | MT6580_AXI_26M);
}

static int mt6580_mainpll_update(struct mt6580_clk_provider *pll, u32 offset,
				u32 clear, u32 set)
{
	u32 value = (readl(pll->base + offset) & ~clear) | set;
	u32 after;

	mt6580_axi_sync();
	writel(value, pll->base + offset);
	after = readl(pll->base + offset);
	mt6580_axi_sync();
	return ((after ^ value) & (clear | set)) ? -EIO : 0;
}

static int mt6580_mainpll_power_down(struct mt6580_clk_provider *pll,
				   struct mt6580_axi_context *ctx)
{
	u32 enable = MT6580_MAINPLL_EN | MT6580_MAINPLL_RSTB;
	u32 power = MT6580_PLL_PWR_ON | MT6580_PLL_ISO_EN;
	int ret;

	/* Set ownership before the first write, so a partial down sequence can
	 * be recovered by the same pre-L2 abort/resume path. AXI is already 26M.
	 */
	ctx->mainpll_off = true;
	ret = mt6580_mainpll_update(pll, MT6580_MAINPLL_CON0, MT6580_MAINPLL_RSTB, 0);
	if (!ret)
		ret = mt6580_mainpll_update(pll, MT6580_MAINPLL_CON0, MT6580_MAINPLL_EN, 0);
	if (!ret)
		ret = mt6580_mainpll_update(pll, MT6580_MAINPLL_PWR_CON, 0, MT6580_PLL_ISO_EN);
	if (!ret)
		ret = mt6580_mainpll_update(pll, MT6580_MAINPLL_PWR_CON, MT6580_PLL_PWR_ON, 0);
	mt6580_mainpll_snapshot(pll, ctx->pll_off);
	if (!ret && ((ctx->pll_off[0] & enable) ||
		     (ctx->pll_off[4] & power) != MT6580_PLL_ISO_EN ||
		     !mt6580_mainpll_config_matches(ctx->pll_off, ctx->pll_before)))
		ret = -EIO;
	if (!ret)
		ctx->power_downs++;
	return ret;
}

static int mt6580_mainpll_power_up(struct mt6580_clk_provider *top,
				 struct mt6580_clk_provider *pll,
				 struct mt6580_axi_context *ctx)
{
	u32 enable = MT6580_MAINPLL_EN | MT6580_MAINPLL_SDM;
	unsigned int i;
	int ret = 0;

	mt6580_mainpll_snapshot(pll, ctx->pll_wake);
	if (mt6580_mainpll_ready(ctx->pll_wake) &&
	    mt6580_mainpll_matches(ctx->pll_wake, ctx->pll_before)) {
		/* Preserve evidence if hardware recovered the PLL before software.
		 * A retry after an earlier software enable is not an automatic wake.
		 */
		ctx->mainpll_auto_restored = !ctx->mainpll_enable_written;
		for (i = 0; i < ARRAY_SIZE(ctx->pll_after); i++)
			ctx->pll_after[i] = ctx->pll_wake[i];
		goto recovered;
	}
	if (!mt6580_mainpll_config_matches(ctx->pll_wake, ctx->pll_before) ||
	    (ctx->cpu_mux_after & MT6580_CPU_MUX_MASK) != MT6580_CPU_ARMPLL) {
		ret = -EIO;
		goto snapshot;
	}
	/* Never issue a power sequence while AXI selects the unverified PLL. */
	if ((readl(top->base) & MT6580_AXI_MUX_MASK) != MT6580_AXI_26M &&
	    (mt6580_axi_keep_26m(top) & MT6580_AXI_MUX_MASK) != MT6580_AXI_26M) {
		ret = -EIO;
		goto snapshot;
	}
	ctx->mainpll_enable_written = true;
	ret = mt6580_mainpll_update(pll, MT6580_MAINPLL_PWR_CON, 0, MT6580_PLL_PWR_ON);
	if (ret)
		goto snapshot;
	udelay(1);
	ret = mt6580_mainpll_update(pll, MT6580_MAINPLL_PWR_CON, MT6580_PLL_ISO_EN, 0);
	if (ret)
		goto snapshot;
	udelay(1);
	ret = mt6580_mainpll_update(pll, MT6580_MAINPLL_CON0, 0, enable);
	if (ret)
		goto snapshot;
	udelay(20);
	ret = mt6580_mainpll_update(pll, MT6580_MAINPLL_CON0, 0, MT6580_MAINPLL_RSTB);
snapshot:
	mt6580_mainpll_snapshot(pll, ctx->pll_after);
	if (ret)
		return ret;
	if (!mt6580_mainpll_ready(ctx->pll_after) ||
	    !mt6580_mainpll_matches(ctx->pll_after, ctx->pll_before))
		return -EIO;
recovered:
	ctx->mainpll_off = false;
	ctx->power_ups++;
	return 0;
}

int mt6580_axi_sleep_prepare(struct device *top, struct device *pll,
			     struct mt6580_axi_context *ctx)
{
	struct mt6580_clk_provider *top_data, *pll_data;
	unsigned int i;
	int ret;

	if (!ctx)
		return -EINVAL;
	if (ctx->prepared || ctx->parked || ctx->mainpll_off)
		return mt6580_axi_error(ctx, -EBUSY);
	ctx->error = 0;
	if (!mt6580_axi_atomic_context())
		return mt6580_axi_error(ctx, -EINVAL);
	ret = mt6580_axi_get_providers(top, pll, &top_data, &pll_data);
	if (ret)
		return mt6580_axi_error(ctx, ret);
	ctx->mux_before = readl(top_data->base);
	ctx->cpu_mux_before = readl(pll_data->infra);
	mt6580_mainpll_snapshot(pll_data, ctx->pll_before);
	ctx->mux_parked = 0;
	ctx->mux_wake = 0;
	ctx->mux_after = 0;
	ctx->cpu_mux_after = 0;
	ctx->mainpll_auto_restored = false;
	ctx->mainpll_enable_written = false;
	ctx->dcm_active = top_data->dcm_owned;
	ctx->dcm_paused = false;
	if (ctx->dcm_active) {
		mt6580_dcm_snapshot(top_data->base, pll_data->infra, ctx->dcm_before);
		if (!mt6580_dcm_matches(ctx->dcm_before, mt6580_dcm_on))
			return mt6580_axi_error(ctx, -EBUSY);
	}
	for (i = 0; i < ARRAY_SIZE(ctx->pll_after); i++) {
		ctx->pll_after[i] = 0;
		ctx->pll_off[i] = 0;
		ctx->pll_wake[i] = 0;
	}
	if ((ctx->mux_before & MT6580_AXI_MUX_MASK) != MT6580_AXI_MPLL_D12 ||
	    (ctx->cpu_mux_before & MT6580_CPU_MUX_MASK) != MT6580_CPU_ARMPLL ||
	    !mt6580_mainpll_ready(ctx->pll_before) ||
	    (ctx->mainpll_requested && !(ctx->pll_before[0] & MT6580_MAINPLL_SDM)))
		return mt6580_axi_error(ctx, -EBUSY);
	ctx->prepared = true;
	return 0;
}
EXPORT_SYMBOL_GPL(mt6580_axi_sleep_prepare);

int mt6580_axi_sleep_enter(struct device *top, struct device *pll,
			   struct mt6580_axi_context *ctx)
{
	struct mt6580_clk_provider *top_data, *pll_data;
	u32 mainpll[5], parked, off[6];
	int ret;

	if (!ctx)
		return -EINVAL;
	if (!ctx->prepared || ctx->parked || ctx->mainpll_off || !mt6580_axi_atomic_context())
		return mt6580_axi_error(ctx, -EINVAL);
	ret = mt6580_axi_get_providers(top, pll, &top_data, &pll_data);
	if (ret)
		return mt6580_axi_error(ctx, ret);
	mt6580_mainpll_snapshot(pll_data, mainpll);
	if (readl(top_data->base) != ctx->mux_before ||
	    readl(pll_data->infra) != ctx->cpu_mux_before ||
	    !mt6580_mainpll_matches(mainpll, ctx->pll_before))
		return mt6580_axi_error(ctx, -EBUSY);
	/* The finisher has left coherency. No CCF or spin lock is usable here;
	 * one online CPU with IRQs disabled owns this bounded mux transaction.
	 * Keep divider enables, audio and all unrelated mux bits intact.
	 * PLL power is touched only by the separately requested sequence below.
	 */
	if (ctx->dcm_active) {
		memcpy(off, ctx->dcm_before, sizeof(off));
		off[0] &= ~BIT(0);
		off[1] &= ~GENMASK(2, 1);
		off[2] &= ~BIT(2);
		off[3] |= BIT(30);
		off[4] &= ~(BIT(11) | BIT(10) | BIT(27) | BIT(26));
		off[5] &= ~(BIT(8) | BIT(14));
		/* Own the partial state before the first write so abort cleanup
		 * still restores it if readback fails before AXI was parked.
		 */
		ctx->dcm_paused = true;
		ctx->dcm_pauses++;
		ret = mt6580_dcm_write(top_data->base, pll_data->infra, off,
					mt6580_dcm_pause_order);
		if (ret)
			return mt6580_axi_error(ctx, ret);
	}
	parked = (ctx->mux_before & ~MT6580_AXI_MUX_MASK) | MT6580_AXI_26M;
	ctx->parked = true;
	ctx->entries++;
	ctx->mux_parked = mt6580_axi_write_mux(top_data, parked);
	if (ctx->mux_parked != parked) {
		ctx->mux_parked = mt6580_axi_keep_26m(top_data);
		return mt6580_axi_error(ctx, -EIO);
	}
	if (ctx->mainpll_requested) {
		ret = mt6580_mainpll_power_down(pll_data, ctx);
		if (ret)
			return mt6580_axi_error(ctx, ret);
	}
	return 0;
}
EXPORT_SYMBOL_GPL(mt6580_axi_sleep_enter);

int mt6580_axi_sleep_resume(struct device *top, struct device *pll,
			    struct mt6580_axi_context *ctx)
{
	struct mt6580_clk_provider *top_data, *pll_data;
	u32 select;
	int ret;

	if (!ctx)
		return -EINVAL;
	/* Also used for aborted preparation and idempotent post-return cleanup. */
	if (!ctx->parked && !ctx->mainpll_off && !ctx->dcm_paused) {
		ctx->prepared = false;
		return 0;
	}
	if (!ctx->prepared || !mt6580_axi_atomic_context())
		return mt6580_axi_error(ctx, -EINVAL);
	ret = mt6580_axi_get_providers(top, pll, &top_data, &pll_data);
	if (ret)
		return mt6580_axi_error(ctx, ret);
	ctx->mux_wake = readl(top_data->base);
	ctx->cpu_mux_after = readl(pll_data->infra);
	if (ctx->mainpll_off) {
		ret = mt6580_mainpll_power_up(top_data, pll_data, ctx);
		if (ret)
			goto keep_26m;
	} else {
		mt6580_mainpll_snapshot(pll_data, ctx->pll_after);
	}
	select = ctx->mux_wake & MT6580_AXI_MUX_MASK;
	if (!mt6580_mainpll_ready(ctx->pll_after) ||
	    !mt6580_mainpll_matches(ctx->pll_after, ctx->pll_before) ||
	    ctx->cpu_mux_after != ctx->cpu_mux_before ||
	    (ctx->cpu_mux_after & MT6580_CPU_MUX_MASK) != MT6580_CPU_ARMPLL ||
	    (ctx->mux_wake & ~MT6580_AXI_MUX_MASK) !=
		(ctx->mux_before & ~MT6580_AXI_MUX_MASK) ||
	    (select != MT6580_AXI_26M && select != MT6580_AXI_MPLL_D12))
		goto keep_26m;
	ctx->mux_after = mt6580_axi_write_mux(top_data, ctx->mux_before);
	if (ctx->mux_after != ctx->mux_before)
		goto keep_26m;
	if (ctx->dcm_paused) {
		ret = mt6580_dcm_write(top_data->base, pll_data->infra, ctx->dcm_before,
					mt6580_dcm_enable_order);
		mt6580_dcm_snapshot(top_data->base, pll_data->infra, ctx->dcm_after);
		if (ret || !mt6580_dcm_matches(ctx->dcm_after, ctx->dcm_before))
			goto keep_26m;
		ctx->dcm_paused = false;
		ctx->dcm_restores++;
	}
	if (ctx->parked)
		ctx->resumes++;
	ctx->parked = false;
	ctx->prepared = false;
	return ctx->error;

keep_26m:
	/* Never reconnect AXI to a PLL with unverified state. Keep ownership
	 * and the original snapshot for a later abort cleanup or diagnosis.
	 */
	ctx->mux_after = mt6580_axi_keep_26m(top_data);
	return mt6580_axi_error(ctx, -EIO);
}
EXPORT_SYMBOL_GPL(mt6580_axi_sleep_resume);

static const struct of_device_id mt6580_usb_clk_match[] = {
	{ .compatible = "mediatek,mt6580-topckgen" },
	{ .compatible = "mediatek,mt6580-apmixedsys" },
	{ .compatible = "mediatek,mt6580-mfgcfg", .data = &mt6580_mfg_desc },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6580_usb_clk_match);

static void mt6580_usb_clk_remove(struct platform_device *pdev)
{
	if (&pdev->dev == READ_ONCE(mt6580_top_device))
		WRITE_ONCE(mt6580_top_device, NULL);
	if (&pdev->dev == READ_ONCE(mt6580_pll_device))
		WRITE_ONCE(mt6580_pll_device, NULL);
	if (of_device_is_compatible(pdev->dev.of_node, "mediatek,mt6580-mfgcfg"))
		mtk_clk_simple_remove(pdev);
}

static struct platform_driver mt6580_usb_clk_driver = {
	.probe = mt6580_usb_clk_probe,
	.remove = mt6580_usb_clk_remove,
	.driver = {
		.name = "mt6580-usb-clocks",
		.of_match_table = mt6580_usb_clk_match,
		.dev_groups = mt6580_clk_groups,
		.pm = pm_sleep_ptr(&mt6580_clk_pm),
	},
};
module_platform_driver(mt6580_usb_clk_driver);

MODULE_DESCRIPTION("MT6580 CPU, peripheral and GPU clocks");
MODULE_LICENSE("GPL");
