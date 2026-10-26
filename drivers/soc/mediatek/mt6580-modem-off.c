// SPDX-License-Identifier: GPL-2.0-only
/* Unused-modem sequence from MediaTek's 2015 MT6580 ccci_off.c.
 * Copyright (C) 2015 MediaTek Inc.
 */
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>

#define RM_TMR_PWR0	0x018
#define RM_TMR_PWR1	0x01c
#define RM_PWR_CON0	0x800
#define RM_PLL_MASK0	0x830
#define RM_PLL_MASK1	0x834
#define SM_REQ_MASK	0x8b0
#define MD_REQ_BITS	GENMASK(5, 0)
#define GSM_REQ_BITS	GENMASK(1, 0)

struct mt6580_modem_off {
	void __iomem *md;
	void __iomem *gsm;
	u32 md_req_before, md_req_after;
	u32 gsm_req_before, gsm_req_after;
	u32 entries;
	bool quiesced;
};

static void mt6580_md_set(void __iomem *addr, u32 bits)
{
	writel(readl(addr) | bits, addr);
}

static void mt6580_md_clear(void __iomem *addr, u32 bits)
{
	writel(readl(addr) & ~bits, addr);
}

static int mt6580_modem_quiesce(struct device *dev)
{
	struct mt6580_modem_off *md = dev_get_drvdata(dev);
	unsigned int i;

	/* genpd has powered MD1 before any access to its private registers.
	 * This node is exclusive to boards with no modem firmware consumer.
	 * Never access these windows once the domain has been released.
	 */
	md->quiesced = false;
	md->md_req_before = readl(md->md + SM_REQ_MASK);
	md->gsm_req_before = readl(md->gsm + SM_REQ_MASK);
	for (i = 0; i < 5; i++)
		mt6580_md_set(md->gsm + RM_PWR_CON0 + 4 * i, 0x44);
	for (i = 0; i < 5; i++) {
		mt6580_md_clear(md->gsm + RM_PWR_CON0 + 4 * i, BIT(7));
		writel(0x200, md->gsm + RM_PWR_CON0 + 4 * i);
	}
	writel(~0U, md->md + SM_REQ_MASK);
	writel(0, md->gsm + RM_TMR_PWR0);
	writel(0, md->gsm + RM_TMR_PWR1);
	for (i = 0; i < 5; i++)
		mt6580_md_clear(md->gsm + RM_PWR_CON0 + 4 * i, 0x44);

	/* ABB isolation and PLL request masks precede MDMCU shutdown. */
	mt6580_md_set(md->md + RM_PWR_CON0, 0x90);
	mt6580_md_set(md->md + RM_PLL_MASK0, 0xffff0000);
	mt6580_md_set(md->md + RM_PLL_MASK1, 0xff);
	writel(~0U, md->gsm + RM_PLL_MASK0);
	mt6580_md_set(md->gsm + RM_PLL_MASK1, 0xf);

	writel(~0U, md->md + SM_REQ_MASK);
	writel(0, md->md + RM_TMR_PWR0);
	writel(0x5229a, md->md + RM_PWR_CON0);
	writel(~0U, md->md + RM_PLL_MASK0);
	writel(~0U, md->md + RM_PLL_MASK1);
	writel(~0U, md->gsm + SM_REQ_MASK);
	writel(~0U, md->gsm + RM_PLL_MASK0);
	writel(~0U, md->gsm + RM_PLL_MASK1);

	md->md_req_after = readl(md->md + SM_REQ_MASK);
	md->gsm_req_after = readl(md->gsm + SM_REQ_MASK);
	/* Reserved request-mask bits read as zero on MT6580 (0x3f/0x03).
	 * Check the implemented fields, not the all-ones write value.
	 */
	if ((md->md_req_after & MD_REQ_BITS) != MD_REQ_BITS ||
	    (md->gsm_req_after & GSM_REQ_BITS) != GSM_REQ_BITS ||
	    readl(md->md + RM_PWR_CON0) != 0x5229a) {
		dev_err(dev, "TOPSM shutdown readback: MD=%#x 2G=%#x power=%#x\n",
			md->md_req_after, md->gsm_req_after,
			readl(md->md + RM_PWR_CON0));
		return -EIO;
	}
	md->quiesced = true;
	md->entries++;
	return 0;
}

static ssize_t state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mt6580_modem_off *md = dev_get_drvdata(dev);

	/* Cached values: the modem register windows are powered off. */
	return sysfs_emit(buf,
		"quiesced=%u entries=%u md_req=%08x/%08x gsm_req=%08x/%08x\n",
		md->quiesced, md->entries, md->md_req_before, md->md_req_after,
		md->gsm_req_before, md->gsm_req_after);
}
static DEVICE_ATTR_RO(state);

static struct attribute *mt6580_modem_attrs[] = {
	&dev_attr_state.attr,
	NULL,
};
ATTRIBUTE_GROUPS(mt6580_modem);

static int mt6580_modem_off_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6580_modem_off *md;
	int ret;

	if (!dev->pm_domain)
		return dev_err_probe(dev, -EINVAL, "MD1 power domain is required\n");
	md = devm_kzalloc(dev, sizeof(*md), GFP_KERNEL);
	if (!md)
		return -ENOMEM;
	md->md = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(md->md))
		return PTR_ERR(md->md);
	md->gsm = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(md->gsm))
		return PTR_ERR(md->gsm);
	platform_set_drvdata(pdev, md);
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Cannot quiesce unused modem\n");
	ret = pm_runtime_put_sync_suspend(dev);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Cannot release MD1 domain\n");
	dev_info(dev, "Unused modem quiesced; MD1 released to genpd\n");
	return 0;
}

static const struct dev_pm_ops mt6580_modem_pm_ops = {
	RUNTIME_PM_OPS(NULL, mt6580_modem_quiesce, NULL)
};

static const struct of_device_id mt6580_modem_off_match[] = {
	{ .compatible = "mediatek,mt6580-modem-off" },
	{ }
};

static struct platform_driver mt6580_modem_off_driver = {
	.probe = mt6580_modem_off_probe,
	.driver = {
		.name = "mt6580-modem-off",
		.of_match_table = mt6580_modem_off_match,
		.pm = pm_ptr(&mt6580_modem_pm_ops),
		.dev_groups = mt6580_modem_groups,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(mt6580_modem_off_driver);
