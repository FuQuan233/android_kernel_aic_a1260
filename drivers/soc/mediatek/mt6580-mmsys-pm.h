/* SPDX-License-Identifier: GPL-2.0-only */
/* MT6580 DISP context, from the BSP SMI and M4U save/restore lists.
 * Included after struct mtk_mmsys. Separate SMI nodes own LARB/COMMON when the DT supplies them;
 * legacy three-resource DTs retain the original context path.
 */

static const u16 mt6580_common_offsets[] = {
	0x100, 0x104, 0x108, 0x10c, 0x110, 0x230, 0x234, 0x300,
};

static const u16 mt6580_larb_offsets[] = {
	0x020, 0x02c,
	0x200, 0x204, 0x208, 0x20c, 0x210, 0x214, 0x218,
	0xfc0, 0xfc4, 0xfd0, 0xfd4, 0xfd8, 0xfdc,
};

static const u16 mt6580_mmsys_offsets[] = {
	0x120, 0x140, 0x144, 0x150, /* DCM, reset banks, LCM reset */
};

struct mt6580_mmsys_pm {
	void __iomem *larb;
	void __iomem *common;
	u32 common_save[ARRAY_SIZE(mt6580_common_offsets)];
	u32 larb_save[ARRAY_SIZE(mt6580_larb_offsets)];
	u32 mmsys_save[ARRAY_SIZE(mt6580_mmsys_offsets)];
	u32 larb_con, cg0, cg1;
	u32 saves, restores, busy;
	bool valid;
	bool bootstrap;
};

static void mt6580_context_save(void __iomem *base, const u16 *offsets,
			       u32 *values, unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++)
		values[i] = readl(base + offsets[i]);
}

static void mt6580_context_restore(void __iomem *base, const u16 *offsets,
				  const u32 *values, unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++)
		writel(values[i], base + offsets[i]);
}

static int mt6580_mmsys_runtime_suspend(struct device *dev)
{
	struct mtk_mmsys *mmsys = dev_get_drvdata(dev);
	struct mt6580_mmsys_pm *pm = mmsys->mt6580_pm;
	u32 status;
	int ret;

	if (!pm)
		return 0;
	/* Children/clock users have stopped. Do not cut an outstanding DMA. */
	ret = pm->larb ? readl_poll_timeout(pm->larb, status, !status, 10, 10000) : 0;
	if (ret) {
		pm->busy++;
		return -EBUSY;
	}
	pm->cg0 = readl(mmsys->regs + 0x100);
	pm->cg1 = readl(mmsys->regs + 0x110);
	if (pm->larb)
		pm->larb_con = readl(pm->larb + 0x10);
	mt6580_context_save(mmsys->regs, mt6580_mmsys_offsets,
			    pm->mmsys_save, ARRAY_SIZE(mt6580_mmsys_offsets));
	if (pm->common)
		mt6580_context_save(pm->common, mt6580_common_offsets,
			    pm->common_save, ARRAY_SIZE(mt6580_common_offsets));
	if (pm->larb)
		mt6580_context_save(pm->larb, mt6580_larb_offsets,
			    pm->larb_save, ARRAY_SIZE(mt6580_larb_offsets));
	pm->valid = true;
	pm->saves++;
	return 0;
}

static int mt6580_mmsys_runtime_resume(struct device *dev)
{
	struct mtk_mmsys *mmsys = dev_get_drvdata(dev);
	struct mt6580_mmsys_pm *pm = mmsys->mt6580_pm;
	u32 status;
	int ret;

	if (!pm || !pm->valid)
		return 0;
	/* genpd has restored SMI's source and SRAM. Open COMMON/LARB before
	 * touching their context; do not call CCF from this clock-provider path.
	 */
	if (pm->larb)
		writel(BIT(0) | BIT(1), mmsys->regs + 0x108);
	mt6580_context_restore(mmsys->regs, mt6580_mmsys_offsets,
			       pm->mmsys_save, ARRAY_SIZE(mt6580_mmsys_offsets));
	ret = pm->larb ? readl_poll_timeout(pm->larb, status, !status, 10, 10000) : 0;
	if (ret)
		return dev_err_probe(dev, ret, "LARB0 busy after DISP power-on\n");
	if (pm->larb) {
		writel(~pm->larb_con, pm->larb + 0x18);
		writel(pm->larb_con, pm->larb + 0x14);
		writel(~0U, pm->larb + 0x64); /* BSP OSTD init, only while idle */
	}
	if (pm->common)
		mt6580_context_restore(pm->common, mt6580_common_offsets,
			       pm->common_save, ARRAY_SIZE(mt6580_common_offsets));
	if (pm->larb)
		mt6580_context_restore(pm->larb, mt6580_larb_offsets,
			       pm->larb_save, ARRAY_SIZE(mt6580_larb_offsets));
	/* Restore gates last. RDMA/mutex/routes/DSI are rebuilt by DRM. */
	writel(~pm->cg0, mmsys->regs + 0x108);
	writel(pm->cg0, mmsys->regs + 0x104);
	writel(~pm->cg1, mmsys->regs + 0x118);
	writel(pm->cg1, mmsys->regs + 0x114);
	readl(mmsys->regs + 0x110);
	pm->restores++;
	return 0;
}

static int mt6580_mmsys_suspend(struct device *dev)
{
	struct mtk_mmsys *mmsys = dev_get_drvdata(dev);

	return mmsys->mt6580_pm ? pm_runtime_force_suspend(dev) : 0;
}

static int mt6580_mmsys_resume(struct device *dev)
{
	struct mtk_mmsys *mmsys = dev_get_drvdata(dev);

	return mmsys->mt6580_pm ? pm_runtime_force_resume(dev) : 0;
}

static const struct dev_pm_ops mt6580_mmsys_pm_ops = {
	RUNTIME_PM_OPS(mt6580_mmsys_runtime_suspend, mt6580_mmsys_runtime_resume, NULL)
	SYSTEM_SLEEP_PM_OPS(mt6580_mmsys_suspend, mt6580_mmsys_resume)
};

/* Counters only: observing a powered-off domain must not access its MMIO. */
static ssize_t power_context_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct mtk_mmsys *mmsys = dev_get_drvdata(dev);
	struct mt6580_mmsys_pm *pm = mmsys->mt6580_pm;

	return sysfs_emit(buf, "saved=%u restored=%u busy=%u valid=%u\n",
			 pm->saves, pm->restores, pm->busy, pm->valid);
}
static DEVICE_ATTR_RO(power_context);
static struct attribute *mt6580_pm_attrs[] = {
	&dev_attr_power_context.attr,
	NULL,
};
static const struct attribute_group mt6580_pm_group = {
	.attrs = mt6580_pm_attrs,
};

static void mt6580_mmsys_drop_bootstrap(void *data)
{
	struct device *dev = data;
	struct mtk_mmsys *mmsys = dev_get_drvdata(dev);

	if (mmsys->mt6580_pm->bootstrap) {
		mmsys->mt6580_pm->bootstrap = false;
		pm_runtime_put_sync(dev);
	}
}

static int mt6580_mmsys_pm_init(struct platform_device *pdev,
			      struct mtk_mmsys *mmsys)
{
	struct device *dev = &pdev->dev;
	struct mt6580_mmsys_pm *pm;
	int ret;

	pm = devm_kzalloc(dev, sizeof(*pm), GFP_KERNEL);
	if (!pm)
		return -ENOMEM;
	if (platform_get_resource(pdev, IORESOURCE_MEM, 1)) {
		pm->larb = devm_platform_ioremap_resource(pdev, 1);
		if (IS_ERR(pm->larb))
			return PTR_ERR(pm->larb);
		pm->common = devm_platform_ioremap_resource(pdev, 2);
		if (IS_ERR(pm->common))
			return PTR_ERR(pm->common);
	}
	mmsys->mt6580_pm = pm;
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		return ret;
	pm->bootstrap = true;
	ret = devm_add_action_or_reset(dev, mt6580_mmsys_drop_bootstrap, dev);
	if (ret)
		return ret;
	return devm_device_add_group(dev, &mt6580_pm_group);
}
