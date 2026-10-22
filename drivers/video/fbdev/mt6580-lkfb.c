// SPDX-License-Identifier: GPL-2.0-only
/* LK scanout handover with explicit diagnostic panel command controls. */
#include <linux/bitfield.h>
#include <linux/fb.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <video/wt6580-panel-init.h>

struct lkfb {
	struct fb_info *info;
	char state[1536];
	u32 palette[16];
	void __iomem *dsi;
	void __iomem *mipi;
	void __iomem *mmsys;
	struct mutex dsi_lock;
	char panel_report[512];
	char panel_state[160];
};

#include "mt6580-lk-dsi.h"

static ssize_t dsistate_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	static const u16 offsets[] = {
		0x000, 0x004, 0x008, 0x00c, 0x010, 0x014, 0x018, 0x01c,
		0x020, 0x024, 0x028, 0x02c, 0x050, 0x054, 0x058, 0x05c,
		0x060, 0x064, 0x068, 0x084, 0x088, 0x090, 0x094, 0x0a0,
		0x0a4, 0x104, 0x108, 0x110, 0x114, 0x118, 0x11c, 0x120,
		0x130, 0x144, 0x148, 0x14c, 0x150, 0x154, 0x158, 0x15c,
		0x160, 0x164, 0x168, 0x16c, 0x170, 0x200, 0x204,
	};
	struct lkfb *fb = dev_get_drvdata(dev);
	unsigned int i;
	int len = 0;

	mutex_lock(&fb->dsi_lock);
	for (i = 0; i < ARRAY_SIZE(offsets); i++)
		len += sysfs_emit_at(buf, len, "DSI %03x=%08x\n",
				     offsets[i], readl(fb->dsi + offsets[i]));
	for (i = 0; i <= 0x98; i += 4)
		len += sysfs_emit_at(buf, len, "MIPI %03x=%08x\n", i, readl(fb->mipi + i));
	len += sysfs_emit_at(buf, len, "MMSYS 150=%08x\n", readl(fb->mmsys + 0x150));
	mutex_unlock(&fb->dsi_lock);
	return len;
}
static DEVICE_ATTR_RO(dsistate);

static ssize_t state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct lkfb *fb = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", fb->state);
}
static DEVICE_ATTR_RO(state);

static struct attribute *lkfb_attrs[] = {
	&dev_attr_state.attr, &dev_attr_panelid.attr, &dev_attr_dsistate.attr,
	&dev_attr_panel.attr, NULL
};
ATTRIBUTE_GROUPS(lkfb);

static int lkfb_setcolreg(unsigned int n, unsigned int r, unsigned int g,
			 unsigned int b, unsigned int a, struct fb_info *info)
{
	u32 *pal = info->pseudo_palette;
	struct fb_var_screeninfo *v = &info->var;

	if (n >= 16)
		return -EINVAL;
	pal[n] = ((r >> (16 - v->red.length)) << v->red.offset) |
		 ((g >> (16 - v->green.length)) << v->green.offset) |
		 ((b >> (16 - v->blue.length)) << v->blue.offset);
	if (v->transp.length)
		pal[n] |= ((1U << v->transp.length) - 1) << v->transp.offset;
	return 0;
}

static const struct fb_ops lkfb_ops = {
	.owner = THIS_MODULE,
	FB_DEFAULT_IOMEM_OPS,
	.fb_setcolreg = lkfb_setcolreg,
};

static int lkfb_format(struct fb_var_screeninfo *v, u32 fmt, bool swap)
{
	v->green.length = 8;
	v->red.length = v->blue.length = 8;
	switch (fmt) {
	case 0:
		v->bits_per_pixel = 16;
		v->red = (struct fb_bitfield) { 11, 5, 0 };
		v->green = (struct fb_bitfield) { 5, 6, 0 };
		v->blue = (struct fb_bitfield) { 0, 5, 0 };
		break;
	case 1:
		v->bits_per_pixel = 24;
		v->red.offset = 16;
		v->green.offset = 8;
		break;
	case 2:
		v->bits_per_pixel = 32;
		v->red.offset = 16;
		v->green.offset = 8;
		v->transp = (struct fb_bitfield) { 24, 8, 0 };
		break;
	case 3:
		v->bits_per_pixel = 32;
		v->red.offset = 24;
		v->green.offset = 16;
		v->blue.offset = 8;
		v->transp = (struct fb_bitfield) { 0, 8, 0 };
		break;
	default:
		return -EINVAL;
	}
	if (swap)
		swap(v->red, v->blue);
	return 0;
}

static int lkfb_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np;
	struct resource ram;
	struct lkfb *fb;
	struct fb_info *info;
	void __iomem *ovl, *rdma, *dsi;
	u32 src, roi, cfg = 0, addr = 0, pitch = 0, size = 0, offset = 0;
	u32 mode, fmt, layer, enable;
	bool swap;
	int ret, len;

	fb = devm_kzalloc(dev, sizeof(*fb), GFP_KERNEL);
	if (!fb)
		return -ENOMEM;
	platform_set_drvdata(pdev, fb);
	ovl = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ovl))
		return PTR_ERR(ovl);
	rdma = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(rdma))
		return PTR_ERR(rdma);
	dsi = devm_platform_ioremap_resource(pdev, 2);
	if (IS_ERR(dsi))
		return PTR_ERR(dsi);
	fb->dsi = dsi;
	fb->mipi = devm_platform_ioremap_resource(pdev, 3);
	if (IS_ERR(fb->mipi))
		return PTR_ERR(fb->mipi);
	fb->mmsys = devm_platform_ioremap_resource(pdev, 4);
	if (IS_ERR(fb->mmsys))
		return PTR_ERR(fb->mmsys);
	mutex_init(&fb->dsi_lock);
	strscpy(fb->panel_report, "Panel not queried; write 1 to panelid to read DCS ID");
	strscpy(fb->panel_state, "Panel inherited from LK; no panel command sent");

	enable = readl(ovl + 0x00c);
	src = readl(ovl + 0x02c) & 0xf;
	roi = readl(ovl + 0x020);
	mode = readl(rdma + 0x010);
	len = scnprintf(fb->state, sizeof(fb->state),
		"OVL en=%08x src=%08x roi=%08x\nRDMA global=%08x size=%08x/%08x con=%08x addr=%08x pitch=%08x\nDSI start=%08x mode=%08x txrx=%08x vact=%08x\n",
		enable, src, roi, mode, readl(rdma + 0x014), readl(rdma + 0x018),
		readl(rdma + 0x024), readl(rdma + 0xf00), readl(rdma + 0x02c),
		readl(dsi), readl(dsi + 0x014), readl(dsi + 0x018), readl(dsi + 0x02c));
	for (layer = 0; layer < 4; layer++) {
		u32 con = readl(ovl + 0x030 + layer * 0x20);
		u32 pa = readl(ovl + 0xf40 + layer * 0x20);
		u32 stride = readl(ovl + 0x044 + layer * 0x20) & 0xffff;
		u32 wh = readl(ovl + 0x038 + layer * 0x20);
		u32 xy = readl(ovl + 0x03c + layer * 0x20);

		len += scnprintf(fb->state + len, sizeof(fb->state) - len,
			"L%u con=%08x addr=%08x pitch=%u size=%08x offset=%08x\n",
			layer, con, pa, stride, wh, xy);
		/* Higher OVL layer numbers are composited above lower ones. */
		if (src & BIT(layer)) {
			cfg = con;
			addr = pa;
			pitch = stride;
			size = wh;
			offset = xy;
		}
	}
	dev_info(dev, "LK display snapshot:\n%s", fb->state);
	/* Expose the top full-screen memory plane without changing composition. */
	if (mode & BIT(1)) {
		cfg = readl(rdma + 0x024);
		if (!(mode & BIT(0)) || (cfg & 3))
			goto unsupported;
		addr = readl(rdma + 0xf00);
		pitch = readl(rdma + 0x02c) & 0xffff;
		size = (readl(rdma + 0x014) & 0x1fff) |
			((readl(rdma + 0x018) & 0x1fff) << 16);
		fmt = FIELD_GET(GENMASK(7, 4), cfg);
		swap = cfg & BIT(8);
	} else {
		if (!(enable & 1) || !src || offset || size != roi ||
		    (cfg & (BIT(30) | GENMASK(29, 28) | BIT(25) | BIT(10) | BIT(9))) ||
		    ((cfg & BIT(8)) && (cfg & 0xff) != 0xff))
			goto unsupported;
		fmt = FIELD_GET(GENMASK(15, 12), cfg);
		swap = cfg & BIT(24);
	}
	if (size != (480 | (800 << 16)) || pitch > 4096)
		goto unsupported;
	np = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!np)
		return -EINVAL;
	ret = of_address_to_resource(np, 0, &ram);
	of_node_put(np);
	if (ret)
		return ret;
	/* Reject DMA/IOMMU addresses outside the known physical LK allocation. */
	if (addr < ram.start || (u64)addr + (u64)pitch * 800 > (u64)ram.end + 1)
		goto unsupported;
	info = framebuffer_alloc(0, dev);
	if (!info)
		return -ENOMEM;
	ret = lkfb_format(&info->var, fmt, swap);
	if (ret || pitch < 480 * info->var.bits_per_pixel / 8) {
		framebuffer_release(info);
		goto unsupported;
	}
	strscpy(info->fix.id, "mt6580-lkfb");
	info->fix.type = FB_TYPE_PACKED_PIXELS;
	info->fix.visual = FB_VISUAL_TRUECOLOR;
	info->fix.smem_start = addr;
	info->fix.smem_len = pitch * 800;
	info->fix.line_length = pitch;
	info->var.xres = info->var.xres_virtual = 480;
	info->var.yres = info->var.yres_virtual = 800;
	info->var.width = info->var.height = -1;
	info->fbops = &lkfb_ops;
	info->pseudo_palette = fb->palette;
	info->screen_base = ioremap_wc(addr, info->fix.smem_len);
	if (!info->screen_base) {
		framebuffer_release(info);
		return -ENOMEM;
	}
	ret = register_framebuffer(info);
	if (ret) {
		iounmap(info->screen_base);
		framebuffer_release(info);
		return ret;
	}
	fb->info = info;
	scnprintf(fb->state + len, sizeof(fb->state) - len,
		  "fb%d: %ux%u-%u addr=%08x pitch=%u fmt=%u swap=%u\n",
		  info->node, 480, 800, info->var.bits_per_pixel, addr, pitch, fmt, swap);
	dev_info(dev, "fb%d: inherited %ux%u-%u at %08x pitch=%u fmt=%u swap=%u\n",
		info->node, 480, 800, info->var.bits_per_pixel, addr, pitch, fmt, swap);
	return 0;

unsupported:
	scnprintf(fb->state + len, sizeof(fb->state) - len,
		  "No framebuffer: unsupported LK scanout; no display registers changed\n");
	dev_warn(dev, "LK scanout not supported; snapshot available, framebuffer writes disabled\n");
	return 0;
}

static void lkfb_remove(struct platform_device *pdev)
{
	struct lkfb *fb = platform_get_drvdata(pdev);

	if (fb->info) {
		unregister_framebuffer(fb->info);
		iounmap(fb->info->screen_base);
		framebuffer_release(fb->info);
	}
}

static const struct of_device_id lkfb_match[] = {
	{ .compatible = "wingtech,wt6580-lkfb" },
	{ }
};
MODULE_DEVICE_TABLE(of, lkfb_match);

static struct platform_driver lkfb_driver = {
	.probe = lkfb_probe,
	.remove = lkfb_remove,
	.driver = {
		.name = "wt6580-lkfb",
		.of_match_table = lkfb_match,
		.dev_groups = lkfb_groups,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(lkfb_driver);
MODULE_DESCRIPTION("WT6580 firmware scanout framebuffer");
MODULE_LICENSE("GPL");
