/* SPDX-License-Identifier: GPL-2.0-only */
/* MT6580 BSP SMI/M4U context; one owner per physical register block. */
static const u16 mt6580_smi_common_offsets[] = {
	0x100, 0x104, 0x108, 0x10c, 0x110, 0x230, 0x234, 0x300,
};
static const u16 mt6580_smi_larb_offsets[] = {
	0x020, 0x02c, 0x200, 0x204, 0x208, 0x20c, 0x210, 0x214, 0x218,
	0xfc0, 0xfc4, 0xfd0, 0xfd4, 0xfd8, 0xfdc,
};

static void mt6580_smi_save(void __iomem *base, const u16 *offsets,
			     u32 *values, unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++)
		values[i] = readl(base + offsets[i]);
}

static void mt6580_smi_restore(void __iomem *base, const u16 *offsets,
				const u32 *values, unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++)
		writel(values[i], base + offsets[i]);
}

static int mt6580_smi_larb_config(struct device *dev)
{
	struct mtk_smi_larb *larb = dev_get_drvdata(dev);
	u32 mmu = larb->mmu ? READ_ONCE(*larb->mmu) : 0;
	u32 val, mask = 0;
	unsigned int port;

	/* Before component binding, capture LK context without changing ports. */
	if (!larb->mmu)
		return 0;
	larb->requested_mmu = mmu;
	if (mmu & ~GENMASK(5, 0))
		return -EINVAL;
	for (port = 0; port < 6; port++)
		if (mmu & BIT(port))
			mask |= 0xf << (4 * port);
	val = readl(larb->base + 0xfd0);
	writel((val & ~mask) | (0x33333333 & mask), larb->base + 0xfd0);
	val = readl(larb->base + 0xfc4);
	writel(val & ~mmu, larb->base + 0xfc4);
	val = readl(larb->base + 0xfc0);
	writel((val & ~GENMASK(5, 0)) | mmu, larb->base + 0xfc0);
	larb->programmed_mmu = readl(larb->base + 0xfc0);
	if ((larb->programmed_mmu & GENMASK(5, 0)) != mmu)
		return -EIO;
	return 0;
}

static ssize_t context_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	if (of_device_is_compatible(dev->of_node, "mediatek,mt6580-smi-common")) {
		struct mtk_smi *smi = dev_get_drvdata(dev);

		return sysfs_emit(buf, "owner=smi-common valid=%u saves=%u restores=%u\n",
				 smi->context_valid, smi->saves, smi->restores);
	} else {
		struct mtk_smi_larb *larb = dev_get_drvdata(dev);

		return sysfs_emit(buf, "owner=larb0 valid=%u saves=%u restores=%u busy=%u mmu=%08x programmed=%08x\n",
			larb->context_valid, larb->saves, larb->restores, larb->busy,
			larb->requested_mmu, larb->programmed_mmu);
	}
}
static DEVICE_ATTR_RO(context);
static struct attribute *mt6580_smi_attrs[] = { &dev_attr_context.attr, NULL };
ATTRIBUTE_GROUPS(mt6580_smi);
