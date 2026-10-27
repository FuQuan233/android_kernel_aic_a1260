// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 MediaTek Inc.
 *
 * Author:
 *  Min Guo <min.guo@mediatek.com>
 *  Yonglong Wu <yonglong.wu@mediatek.com>
 */

#include <linux/clk.h>
#include <linux/dma-mapping.h>
#include <linux/mfd/mt6350/usb.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/soc/mediatek/mt6580-usb.h>
#include <linux/usb/role.h>
#include <linux/usb/usb_phy_generic.h>
#include "musb_core.h"
#include "musb_dma.h"

#define USB_L1INTS		0x00a0
#define USB_L1INTM		0x00a4
#define MTK_MUSB_TXFUNCADDR	0x0480
#define MT6580_MUSB_SWRST		0x0074
#define MT6580_DISUSBRESET	BIT(0)
#define MT6580_SWRST		BIT(1)
#define MT6580_GADGET_HANDOFF_MS	5000

/* MediaTek controller toggle enable and status reg */
#define MUSB_RXTOG		0x80
#define MUSB_RXTOGEN		0x82
#define MUSB_TXTOG		0x84
#define MUSB_TXTOGEN		0x86
#define MTK_TOGGLE_EN		GENMASK(15, 0)

#define TX_INT_STATUS		BIT(0)
#define RX_INT_STATUS		BIT(1)
#define USBCOM_INT_STATUS	BIT(2)
#define DMA_INT_STATUS		BIT(3)

#define DMA_INTR_STATUS_MSK	GENMASK(7, 0)
#define DMA_INTR_UNMASK_SET_MSK	GENMASK(31, 24)

#define MTK_MUSB_CLKS_NUM	3

struct mtk_glue {
	struct device *dev;
	struct musb *musb;
	struct platform_device *musb_pdev;
	struct platform_device *usb_phy;
	struct phy *phy;
	struct usb_phy *xceiv;
	enum phy_mode phy_mode;
	struct clk_bulk_data clks[MTK_MUSB_CLKS_NUM];
	enum usb_role role;
	struct usb_role_switch *role_sw;
	struct device *vbus_detector;
	bool mt6580;
	struct wakeup_source *enumeration_ws;
	bool clocks_gated, phy_suspended, pm_masked;
	u32 pm_l1_mask, pm_sleeps, pm_resumes, pm_skips;
	u32 pm_bound, pm_speed;
	bool pm_vbus_ready;
	int pm_error;
};

static int mtk_musb_clks_get(struct mtk_glue *glue)
{
	struct device *dev = glue->dev;

	glue->clks[0].id = "main";
	glue->clks[1].id = "mcu";
	glue->clks[2].id = "univpll";

	return devm_clk_bulk_get(dev, MTK_MUSB_CLKS_NUM, glue->clks);
}

static int mtk_otg_switch_set(struct mtk_glue *glue, enum usb_role role)
{
	struct musb *musb = glue->musb;
	u8 devctl = readb(musb->mregs + MUSB_DEVCTL);
	enum usb_role new_role;

	if (role == glue->role)
		return 0;

	switch (role) {
	case USB_ROLE_HOST:
		musb->xceiv->otg->state = OTG_STATE_A_WAIT_VRISE;
		glue->phy_mode = PHY_MODE_USB_HOST;
		new_role = USB_ROLE_HOST;
		if (glue->role == USB_ROLE_NONE)
			phy_power_on(glue->phy);

		devctl |= MUSB_DEVCTL_SESSION;
		musb_writeb(musb->mregs, MUSB_DEVCTL, devctl);
		MUSB_HST_MODE(musb);
		break;
	case USB_ROLE_DEVICE:
		musb->xceiv->otg->state = OTG_STATE_B_IDLE;
		glue->phy_mode = PHY_MODE_USB_DEVICE;
		new_role = USB_ROLE_DEVICE;
		devctl &= ~MUSB_DEVCTL_SESSION;
		musb_writeb(musb->mregs, MUSB_DEVCTL, devctl);
		if (glue->role == USB_ROLE_NONE)
			phy_power_on(glue->phy);

		MUSB_DEV_MODE(musb);
		break;
	case USB_ROLE_NONE:
		glue->phy_mode = PHY_MODE_USB_OTG;
		new_role = USB_ROLE_NONE;
		devctl &= ~MUSB_DEVCTL_SESSION;
		musb_writeb(musb->mregs, MUSB_DEVCTL, devctl);
		if (glue->role != USB_ROLE_NONE)
			phy_power_off(glue->phy);

		break;
	default:
		dev_err(glue->dev, "Invalid State\n");
		return -EINVAL;
	}

	glue->role = new_role;
	phy_set_mode(glue->phy, glue->phy_mode);

	return 0;
}

static int musb_usb_role_sx_set(struct usb_role_switch *sw, enum usb_role role)
{
	return mtk_otg_switch_set(usb_role_switch_get_drvdata(sw), role);
}

static enum usb_role musb_usb_role_sx_get(struct usb_role_switch *sw)
{
	struct mtk_glue *glue = usb_role_switch_get_drvdata(sw);

	return glue->role;
}

static int mtk_otg_switch_init(struct mtk_glue *glue)
{
	struct usb_role_switch_desc role_sx_desc = { 0 };

	role_sx_desc.set = musb_usb_role_sx_set;
	role_sx_desc.get = musb_usb_role_sx_get;
	role_sx_desc.allow_userspace_control = true;
	role_sx_desc.fwnode = dev_fwnode(glue->dev);
	role_sx_desc.driver_data = glue;
	glue->role_sw = usb_role_switch_register(glue->dev, &role_sx_desc);

	return PTR_ERR_OR_ZERO(glue->role_sw);
}

static void mtk_otg_switch_exit(struct mtk_glue *glue)
{
	return usb_role_switch_unregister(glue->role_sw);
}

static irqreturn_t generic_interrupt(int irq, void *__hci)
{
	unsigned long flags;
	irqreturn_t retval = IRQ_NONE;
	struct musb *musb = __hci;
	struct mtk_glue *glue = dev_get_drvdata(musb->controller->parent);

	spin_lock_irqsave(&musb->lock, flags);
	musb->int_usb = musb_clearb(musb->mregs, MUSB_INTRUSB);
	musb->int_rx = musb_clearw(musb->mregs, MUSB_INTRRX);
	musb->int_tx = musb_clearw(musb->mregs, MUSB_INTRTX);

	/*
	 * The MT6580 device PHY cannot preserve an active link over system
	 * suspend. Match the BSP USB suspend lock, without holding the screen
	 * on or preventing sleep on a charger with no USB data connection.
	 * Follow the core's RESUME -> SUSPEND/DISCONNECT -> RESET ordering.
	 */
	if (glue->mt6580 && !is_host_active(musb)) {
		if (musb->int_usb & MUSB_INTR_RESUME) {
			pm_stay_awake(musb->controller);
			__pm_wakeup_event(glue->enumeration_ws, MT6580_GADGET_HANDOFF_MS);
		}
		if (musb->int_usb & (MUSB_INTR_SUSPEND | MUSB_INTR_DISCONNECT)) {
			/* RESET only starts enumeration; it does not prove the
			 * replacement FunctionFS gadget is configured. A host may
			 * suspend that unconfigured link while handling descriptors.
			 * Keep a bounded guard across that gap, without extending
			 * normal configured-link suspend or physical disconnect.
			 */
			if ((musb->int_usb & MUSB_INTR_SUSPEND) &&
			    !(musb->int_usb & MUSB_INTR_DISCONNECT) &&
			    musb->gadget_driver && musb->g.state != USB_STATE_CONFIGURED)
				__pm_wakeup_event(glue->enumeration_ws, MT6580_GADGET_HANDOFF_MS);
			pm_relax(musb->controller);
		}
		if (musb->int_usb & MUSB_INTR_RESET) {
			pm_stay_awake(musb->controller);
			__pm_wakeup_event(glue->enumeration_ws, MT6580_GADGET_HANDOFF_MS);
		}
	}

	if ((musb->int_usb & MUSB_INTR_RESET) && !is_host_active(musb)) {
		/* ep0 FADDR must be 0 when (re)entering peripheral mode */
		musb_ep_select(musb->mregs, 0);
		musb_writeb(musb->mregs, MUSB_FADDR, 0);
	}

	if (musb->int_usb || musb->int_tx || musb->int_rx)
		retval = musb_interrupt(musb);

	spin_unlock_irqrestore(&musb->lock, flags);

	return retval;
}

static irqreturn_t mtk_musb_interrupt(int irq, void *dev_id)
{
	irqreturn_t retval = IRQ_NONE;
	struct musb *musb = (struct musb *)dev_id;
	struct mtk_glue *glue = dev_get_drvdata(musb->controller->parent);
	u32 l1_ints;

	if (READ_ONCE(glue->clocks_gated))
		return IRQ_NONE;
	l1_ints = musb_readl(musb->mregs, USB_L1INTS) &
			musb_readl(musb->mregs, USB_L1INTM);

	if (l1_ints & (TX_INT_STATUS | RX_INT_STATUS | USBCOM_INT_STATUS))
		retval = generic_interrupt(irq, musb);

#if defined(CONFIG_USB_INVENTRA_DMA)
	if (l1_ints & DMA_INT_STATUS)
		retval = dma_controller_irq(irq, musb->dma_controller);
#endif
	return retval;
}

static u32 mtk_musb_busctl_offset(u8 epnum, u16 offset)
{
	return MTK_MUSB_TXFUNCADDR + offset + 8 * epnum;
}

static u8 mtk_musb_clearb(void __iomem *addr, unsigned int offset)
{
	u8 data;

	/* W1C */
	data = musb_readb(addr, offset);
	musb_writeb(addr, offset, data);
	return data;
}

static u16 mtk_musb_clearw(void __iomem *addr, unsigned int offset)
{
	u16 data;

	/* W1C */
	data = musb_readw(addr, offset);
	musb_writew(addr, offset, data);
	return data;
}

static int mtk_musb_set_mode(struct musb *musb, u8 mode)
{
	struct device *dev = musb->controller;
	struct mtk_glue *glue = dev_get_drvdata(dev->parent);
	enum phy_mode new_mode;
	enum usb_role new_role;

	switch (mode) {
	case MUSB_HOST:
		new_mode = PHY_MODE_USB_HOST;
		new_role = USB_ROLE_HOST;
		break;
	case MUSB_PERIPHERAL:
		new_mode = PHY_MODE_USB_DEVICE;
		new_role = USB_ROLE_DEVICE;
		break;
	case MUSB_OTG:
		new_mode = PHY_MODE_USB_OTG;
		new_role = USB_ROLE_NONE;
		break;
	default:
		dev_err(glue->dev, "Invalid mode request\n");
		return -EINVAL;
	}

	if (glue->phy_mode == new_mode)
		return 0;

	if (musb->port_mode != MUSB_OTG) {
		dev_err(glue->dev, "Does not support changing modes\n");
		return -EINVAL;
	}

	mtk_otg_switch_set(glue, new_role);
	return 0;
}

static int mtk_musb_init(struct musb *musb)
{
	struct device *dev = musb->controller;
	struct mtk_glue *glue = dev_get_drvdata(dev->parent);
	int ret;

	glue->musb = musb;
	musb->phy = glue->phy;
	musb->xceiv = glue->xceiv;
	musb->is_host = false;
	musb->isr = mtk_musb_interrupt;

	/* Set TX/RX toggle enable */
	musb_writew(musb->mregs, MUSB_TXTOGEN, MTK_TOGGLE_EN);
	musb_writew(musb->mregs, MUSB_RXTOGEN, MTK_TOGGLE_EN);

	if (musb->port_mode == MUSB_OTG) {
		ret = mtk_otg_switch_init(glue);
		if (ret)
			return ret;
	}

	ret = phy_init(glue->phy);
	if (ret)
		goto err_phy_init;

	ret = phy_power_on(glue->phy);
	if (ret)
		goto err_phy_power_on;

	phy_set_mode(glue->phy, glue->phy_mode);

	if (of_device_is_compatible(dev->parent->of_node, "mediatek,mt6580-musb"))
		musb_writew(musb->mregs, MT6580_MUSB_SWRST,
			    musb_readw(musb->mregs, MT6580_MUSB_SWRST) |
			    MT6580_DISUSBRESET | MT6580_SWRST);

#if defined(CONFIG_USB_INVENTRA_DMA)
	musb_writel(musb->mregs, MUSB_HSDMA_INTR,
		    DMA_INTR_STATUS_MSK | DMA_INTR_UNMASK_SET_MSK);
#endif
	musb_writel(musb->mregs, USB_L1INTM, TX_INT_STATUS | RX_INT_STATUS |
		    USBCOM_INT_STATUS |
		    (IS_ENABLED(CONFIG_USB_INVENTRA_DMA) ? DMA_INT_STATUS : 0));
	return 0;

err_phy_power_on:
	phy_exit(glue->phy);
err_phy_init:
	if (musb->port_mode == MUSB_OTG)
		mtk_otg_switch_exit(glue);
	return ret;
}

static u16 mtk_musb_get_toggle(struct musb_qh *qh, int is_out)
{
	struct musb *musb = qh->hw_ep->musb;
	u8 epnum = qh->hw_ep->epnum;
	u16 toggle;

	toggle = musb_readw(musb->mregs, is_out ? MUSB_TXTOG : MUSB_RXTOG);
	return toggle & (1 << epnum);
}

static u16 mtk_musb_set_toggle(struct musb_qh *qh, int is_out, struct urb *urb)
{
	struct musb *musb = qh->hw_ep->musb;
	u8 epnum = qh->hw_ep->epnum;
	u16 value, toggle;

	toggle = usb_gettoggle(urb->dev, qh->epnum, is_out);

	if (is_out) {
		value = musb_readw(musb->mregs, MUSB_TXTOG);
		value |= toggle << epnum;
		musb_writew(musb->mregs, MUSB_TXTOG, value);
	} else {
		value = musb_readw(musb->mregs, MUSB_RXTOG);
		value |= toggle << epnum;
		musb_writew(musb->mregs, MUSB_RXTOG, value);
	}

	return 0;
}

static void mtk_musb_enable(struct musb *musb)
{
	struct mtk_glue *glue = dev_get_drvdata(musb->controller->parent);

	/* Normal child resume has now restored the core context and IRQ masks.
	 * Do not unmask the wrapper earlier, while PHY recovery can latch IRQs.
	 */
	if (glue->pm_masked && !glue->clocks_gated && !glue->phy_suspended) {
		musb_writel(musb->mregs, USB_L1INTM, glue->pm_l1_mask);
		glue->pm_masked = false;
	}
	/* Keep the enumeration window awake through RESET and configuration.
	 * A RESET alone does not prove a working host link. With no host the
	 * timeout permits sleep.
	 */
	if (glue->enumeration_ws && musb->gadget_driver && !is_host_active(musb))
		__pm_wakeup_event(glue->enumeration_ws, MT6580_GADGET_HANDOFF_MS);
}

static void mtk_musb_disable(struct musb *musb)
{
	struct mtk_glue *glue = dev_get_drvdata(musb->controller->parent);

	if (!glue->mt6580)
		return;
	/* gadget_stop sets UNDEFINED before musb_stop, while gadget_driver
	 * is still attached. FunctionFS teardown can immediately be followed
	 * by a new adbd. Releasing the last wake reference here lets Android
	 * freeze the replacement daemon before it can publish descriptors.
	 * This bounded handoff is not used by system suspend or host mode.
	 */
	if (!is_host_active(musb) && musb->gadget_driver &&
	    musb_get_state(musb) == OTG_STATE_UNDEFINED)
		__pm_wakeup_event(glue->enumeration_ws, MT6580_GADGET_HANDOFF_MS);
	pm_relax(musb->controller);
}

static int mtk_musb_exit(struct musb *musb)
{
	struct device *dev = musb->controller;
	struct mtk_glue *glue = dev_get_drvdata(dev->parent);

	if (glue->mt6580) {
		__pm_relax(glue->enumeration_ws);
		pm_relax(dev);
	}
	mtk_otg_switch_exit(glue);
	phy_power_off(glue->phy);
	phy_exit(glue->phy);
	clk_bulk_disable_unprepare(MTK_MUSB_CLKS_NUM, glue->clks);

	pm_runtime_put_sync(dev);
	pm_runtime_disable(dev);
	return 0;
}

static const struct musb_platform_ops mtk_musb_ops = {
	.quirks = MUSB_DMA_INVENTRA,
	.init = mtk_musb_init,
	.enable = mtk_musb_enable,
	.disable = mtk_musb_disable,
	.get_toggle = mtk_musb_get_toggle,
	.set_toggle = mtk_musb_set_toggle,
	.exit = mtk_musb_exit,
#ifdef CONFIG_USB_INVENTRA_DMA
	.dma_init = musbhs_dma_controller_create_noirq,
	.dma_exit = musbhs_dma_controller_destroy,
#endif
	.clearb = mtk_musb_clearb,
	.clearw = mtk_musb_clearw,
	.busctl_offset = mtk_musb_busctl_offset,
	.set_mode = mtk_musb_set_mode,
};

#define MTK_MUSB_MAX_EP_NUM	8
#define MTK_MUSB_RAM_BITS	11

static const struct musb_fifo_cfg mtk_musb_mode_cfg[] = {
	{ .hw_ep_num = 1, .style = FIFO_TX, .maxpacket = 512, },
	{ .hw_ep_num = 1, .style = FIFO_RX, .maxpacket = 512, },
	{ .hw_ep_num = 2, .style = FIFO_TX, .maxpacket = 512, },
	{ .hw_ep_num = 2, .style = FIFO_RX, .maxpacket = 512, },
	{ .hw_ep_num = 3, .style = FIFO_TX, .maxpacket = 512, },
	{ .hw_ep_num = 3, .style = FIFO_RX, .maxpacket = 512, },
	{ .hw_ep_num = 4, .style = FIFO_TX, .maxpacket = 512, },
	{ .hw_ep_num = 4, .style = FIFO_RX, .maxpacket = 512, },
	{ .hw_ep_num = 5, .style = FIFO_TX, .maxpacket = 512, },
	{ .hw_ep_num = 5, .style = FIFO_RX, .maxpacket = 512, },
	{ .hw_ep_num = 6, .style = FIFO_TX, .maxpacket = 1024, },
	{ .hw_ep_num = 6, .style = FIFO_RX, .maxpacket = 1024, },
	{ .hw_ep_num = 7, .style = FIFO_TX, .maxpacket = 512, },
	{ .hw_ep_num = 7, .style = FIFO_RX, .maxpacket = 64, },
};

/*
 * MT6580 has 8 KiB of FIFO RAM. Its vendor driver uses two 512-byte
 * buffers for bulk EP1. Keep that layout for sustained gadget traffic;
 * reducing EP6 to one 512-byte buffer per direction keeps the total,
 * including the 64-byte EP0 FIFO, at 7808 bytes.
 */
static const struct musb_fifo_cfg mt6580_musb_mode_cfg[] = {
	{ .hw_ep_num = 1, .style = FIFO_TX, .maxpacket = 512, .mode = BUF_DOUBLE, },
	{ .hw_ep_num = 1, .style = FIFO_RX, .maxpacket = 512, .mode = BUF_DOUBLE, },
	{ .hw_ep_num = 2, .style = FIFO_TX, .maxpacket = 512, },
	{ .hw_ep_num = 2, .style = FIFO_RX, .maxpacket = 512, },
	{ .hw_ep_num = 3, .style = FIFO_TX, .maxpacket = 512, },
	{ .hw_ep_num = 3, .style = FIFO_RX, .maxpacket = 512, },
	{ .hw_ep_num = 4, .style = FIFO_TX, .maxpacket = 512, },
	{ .hw_ep_num = 4, .style = FIFO_RX, .maxpacket = 512, },
	{ .hw_ep_num = 5, .style = FIFO_TX, .maxpacket = 512, },
	{ .hw_ep_num = 5, .style = FIFO_RX, .maxpacket = 512, },
	{ .hw_ep_num = 6, .style = FIFO_TX, .maxpacket = 512, },
	{ .hw_ep_num = 6, .style = FIFO_RX, .maxpacket = 512, },
	{ .hw_ep_num = 7, .style = FIFO_TX, .maxpacket = 512, },
	{ .hw_ep_num = 7, .style = FIFO_RX, .maxpacket = 64, },
};

static const struct musb_hdrc_config mt6580_musb_hdrc_config = {
	.fifo_cfg = mt6580_musb_mode_cfg,
	.fifo_cfg_size = ARRAY_SIZE(mt6580_musb_mode_cfg),
	.multipoint = true,
	.dyn_fifo = true,
	.num_eps = MTK_MUSB_MAX_EP_NUM,
	.ram_bits = MTK_MUSB_RAM_BITS,
};

static const struct musb_hdrc_config mtk_musb_hdrc_config = {
	.fifo_cfg = mtk_musb_mode_cfg,
	.fifo_cfg_size = ARRAY_SIZE(mtk_musb_mode_cfg),
	.multipoint = true,
	.dyn_fifo = true,
	.num_eps = MTK_MUSB_MAX_EP_NUM,
	.ram_bits = MTK_MUSB_RAM_BITS,
};

static const struct platform_device_info mtk_dev_info = {
	.name = "musb-hdrc",
	.id = PLATFORM_DEVID_AUTO,
	.dma_mask = DMA_BIT_MASK(32),
};

static void mtk_musb_unregister_enumeration_ws(void *data)
{
	wakeup_source_unregister(data);
}

static void mtk_musb_put_vbus_detector(void *data)
{
	put_device(data);
}

static int mtk_musb_get_vbus_detector(struct mtk_glue *glue)
{
	struct device *dev = glue->dev;
	struct platform_device *pdev;
	struct device_node *node;
	int ret;

	if (!glue->mt6580 || !of_property_present(dev->of_node, "extcon"))
		return 0;
	node = of_parse_phandle(dev->of_node, "extcon", 0);
	if (!of_device_is_compatible(node, "mediatek,mt6350-usb") ||
	    !of_device_is_available(node)) {
		of_node_put(node);
		return -EINVAL;
	}
	pdev = of_find_device_by_node(node);
	of_node_put(node);
	if (!pdev)
		return -EPROBE_DEFER;
	if (!device_is_bound(&pdev->dev)) {
		put_device(&pdev->dev);
		return -EPROBE_DEFER;
	}
	ret = devm_add_action_or_reset(dev, mtk_musb_put_vbus_detector, &pdev->dev);
	if (ret)
		return ret;
	if (!device_link_add(dev, &pdev->dev, DL_FLAG_AUTOREMOVE_CONSUMER))
		return -EINVAL;
	glue->vbus_detector = &pdev->dev;
	return 0;
}

static int mtk_musb_probe(struct platform_device *pdev)
{
	struct musb_hdrc_platform_data *pdata;
	struct mtk_glue *glue;
	struct platform_device_info pinfo;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	int ret;

	glue = devm_kzalloc(dev, sizeof(*glue), GFP_KERNEL);
	if (!glue)
		return -ENOMEM;

	glue->dev = dev;
	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	ret = of_platform_populate(np, NULL, NULL, dev);
	if (ret)
		return dev_err_probe(dev, ret,
				"failed to create child devices at %p\n", np);

	ret = mtk_musb_clks_get(glue);
	if (ret)
		return ret;

	glue->mt6580 = of_device_is_compatible(np, "mediatek,mt6580-musb");
	ret = mtk_musb_get_vbus_detector(glue);
	if (ret)
		return dev_err_probe(dev, ret, "VBUS wake detector unavailable\n");
	if (glue->mt6580) {
		pdata->config = &mt6580_musb_hdrc_config;
		dev_info(dev, "MT6580 8 KiB FIFO: bulk EP1 double buffered\n");
	} else {
		pdata->config = &mtk_musb_hdrc_config;
	}
	pdata->platform_ops = &mtk_musb_ops;
	pdata->mode = usb_get_dr_mode(dev);

	if (IS_ENABLED(CONFIG_USB_MUSB_HOST))
		pdata->mode = USB_DR_MODE_HOST;
	else if (IS_ENABLED(CONFIG_USB_MUSB_GADGET))
		pdata->mode = USB_DR_MODE_PERIPHERAL;

	switch (pdata->mode) {
	case USB_DR_MODE_HOST:
		glue->phy_mode = PHY_MODE_USB_HOST;
		glue->role = USB_ROLE_HOST;
		break;
	case USB_DR_MODE_PERIPHERAL:
		glue->phy_mode = PHY_MODE_USB_DEVICE;
		glue->role = USB_ROLE_DEVICE;
		break;
	case USB_DR_MODE_OTG:
		glue->phy_mode = PHY_MODE_USB_OTG;
		glue->role = USB_ROLE_NONE;
		break;
	default:
		return dev_err_probe(&pdev->dev, -EINVAL,
				"Error 'dr_mode' property\n");
	}

	glue->phy = devm_of_phy_get_by_index(dev, np, 0);
	if (IS_ERR(glue->phy))
		return dev_err_probe(dev, PTR_ERR(glue->phy),
				"fail to getting phy\n");

	glue->usb_phy = usb_phy_generic_register();
	if (IS_ERR(glue->usb_phy))
		return dev_err_probe(dev, PTR_ERR(glue->usb_phy),
				"fail to registering usb-phy\n");

	glue->xceiv = devm_usb_get_phy(dev, USB_PHY_TYPE_USB2);
	if (IS_ERR(glue->xceiv)) {
		ret = PTR_ERR(glue->xceiv);
		dev_err(dev, "fail to getting usb-phy %d\n", ret);
		goto err_unregister_usb_phy;
	}

	platform_set_drvdata(pdev, glue);
	pm_runtime_enable(dev);
	pm_runtime_get_sync(dev);

	ret = clk_bulk_prepare_enable(MTK_MUSB_CLKS_NUM, glue->clks);
	if (ret)
		goto err_enable_clk;

	if (IS_ENABLED(CONFIG_PM_SLEEP) && glue->mt6580 &&
	    pdata->mode == USB_DR_MODE_PERIPHERAL) {
		/* Separate the timeout from the active-link source: starting a
		 * bounded handoff must not shorten an existing RESET/RESUME hold.
		 */
		glue->enumeration_ws = wakeup_source_register(dev, "mt6580-usb-enumeration");
		if (!glue->enumeration_ws) {
			ret = -ENOMEM;
			goto err_device_register;
		}
		ret = devm_add_action_or_reset(dev, mtk_musb_unregister_enumeration_ws,
					       glue->enumeration_ws);
		if (ret)
			goto err_device_register;
	}

	pinfo = mtk_dev_info;
	pinfo.parent = dev;
	pinfo.res = pdev->resource;
	pinfo.num_res = pdev->num_resources;
	pinfo.data = pdata;
	pinfo.size_data = sizeof(*pdata);
	pinfo.fwnode = of_fwnode_handle(np);
	pinfo.of_node_reused = true;

	glue->musb_pdev = platform_device_register_full(&pinfo);
	if (IS_ERR(glue->musb_pdev)) {
		ret = PTR_ERR(glue->musb_pdev);
		dev_err(dev, "failed to register musb device: %d\n", ret);
		goto err_device_register;
	}

	return 0;

err_device_register:
	clk_bulk_disable_unprepare(MTK_MUSB_CLKS_NUM, glue->clks);
err_enable_clk:
	pm_runtime_put_sync(dev);
	pm_runtime_disable(dev);
err_unregister_usb_phy:
	usb_phy_generic_unregister(glue->usb_phy);
	return ret;
}

static void mtk_musb_remove(struct platform_device *pdev)
{
	struct mtk_glue *glue = platform_get_drvdata(pdev);
	struct platform_device *usb_phy = glue->usb_phy;

	platform_device_unregister(glue->musb_pdev);
	usb_phy_generic_unregister(usb_phy);
}

static int mtk_musb_suspend_late(struct device *dev)
{
	struct mtk_glue *glue = dev_get_drvdata(dev);
	struct musb *musb = glue->musb;
	unsigned long flags;
	int ret;

	if (!glue->mt6580 || !musb || musb->port_mode != MUSB_PERIPHERAL)
		return 0;
	glue->pm_error = 0;
	spin_lock_irqsave(&musb->lock, flags);
	/* A bound gadget may lose its PHY only with verified absent VBUS and
	 * an armed, independent PMIC insertion IRQ. Errors, an online cable
	 * or a missing/disabled detector retain the listening PHY. Active links
	 * still use their existing wake source.
	 */
	glue->pm_bound = !!musb->gadget_driver;
	glue->pm_speed = musb->g.speed;
	glue->pm_vbus_ready = glue->vbus_detector &&
		mt6350_usb_suspend_ready(glue->vbus_detector);
	/* MUSB stop clears gadget_driver but can retain the old negotiated
	 * speed. That stale speed does not represent a link after unbind.
	 */
	if (glue->pm_bound && !glue->pm_vbus_ready) {
		glue->pm_skips++;
		spin_unlock_irqrestore(&musb->lock, flags);
		return 0;
	}
	/* The child's normal suspend saved context and disabled core IRQs. */
	if (!glue->pm_masked)
		glue->pm_l1_mask = musb_readl(musb->mregs, USB_L1INTM);
	musb_writel(musb->mregs, USB_L1INTM, 0);
	glue->pm_masked = true;
	spin_unlock_irqrestore(&musb->lock, flags);
	synchronize_irq(musb->nIrq);
	if (!glue->phy_suspended) {
		ret = phy_power_off(glue->phy);
		if (ret) {
			musb_writel(musb->mregs, USB_L1INTM, glue->pm_l1_mask);
			glue->pm_masked = false;
			glue->pm_error = ret;
			return ret;
		}
		glue->phy_suspended = true;
	}
	WRITE_ONCE(glue->clocks_gated, true);
	clk_bulk_disable_unprepare(MTK_MUSB_CLKS_NUM, glue->clks);
	glue->pm_sleeps++;
	return 0;
}

static int mtk_musb_resume_early(struct device *dev)
{
	struct mtk_glue *glue = dev_get_drvdata(dev);
	int ret;

	if (!glue->clocks_gated && !glue->phy_suspended)
		return 0;
	if (glue->clocks_gated) {
		ret = clk_bulk_prepare_enable(MTK_MUSB_CLKS_NUM, glue->clks);
		if (ret)
			goto error;
		WRITE_ONCE(glue->clocks_gated, false);
	}
	/* Restore clocks/PHY before the child's normal MMIO context restore. */
	ret = phy_power_on(glue->phy);
	if (ret)
		goto error;
	glue->phy_suspended = false;
	glue->pm_resumes++;
	return 0;
error:
	glue->pm_error = ret;
	dev_err(dev, "USB clocks/PHY resume failed: %d\n", ret);
	return ret;
}

static const struct dev_pm_ops mtk_musb_pm_ops = {
	LATE_SYSTEM_SLEEP_PM_OPS(mtk_musb_suspend_late, mtk_musb_resume_early)
};

static ssize_t pm_state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mtk_glue *glue = dev_get_drvdata(dev);

	if (!glue->mt6580)
		return -ENODEV;
	/* Cached: reading this attribute never wakes or touches the USB core. */
	return sysfs_emit(buf,
		"gated=%u masked=%u sleeps=%u resumes=%u skips=%u error=%d "
		"l1_mask=%08x phy_off=%u bound=%u speed=%u vbus_ready=%u\n",
		glue->clocks_gated, glue->pm_masked, glue->pm_sleeps,
		glue->pm_resumes, glue->pm_skips, glue->pm_error, glue->pm_l1_mask,
		glue->phy_suspended, glue->pm_bound, glue->pm_speed, glue->pm_vbus_ready);
}
static DEVICE_ATTR_RO(pm_state);

static struct attribute *mtk_musb_attrs[] = {
	&dev_attr_pm_state.attr,
	NULL,
};
ATTRIBUTE_GROUPS(mtk_musb);

#ifdef CONFIG_OF
static const struct of_device_id mtk_musb_match[] = {
	{.compatible = "mediatek,mtk-musb",},
	{},
};
MODULE_DEVICE_TABLE(of, mtk_musb_match);
#endif

static struct platform_driver mtk_musb_driver = {
	.probe = mtk_musb_probe,
	.remove = mtk_musb_remove,
	.driver = {
		   .name = "musb-mtk",
		   .of_match_table = of_match_ptr(mtk_musb_match),
		   .pm = pm_ptr(&mtk_musb_pm_ops),
		   .dev_groups = mtk_musb_groups,
	},
};

bool mt6580_usb_sleep_ready(struct device *dev)
{
	struct mtk_glue *glue;

	if (dev->driver != &mtk_musb_driver.driver)
		return false;
	glue = dev_get_drvdata(dev);
	return glue->mt6580 && READ_ONCE(glue->clocks_gated) &&
		READ_ONCE(glue->phy_suspended) && READ_ONCE(glue->pm_masked) &&
		!READ_ONCE(glue->pm_error);
}
EXPORT_SYMBOL_GPL(mt6580_usb_sleep_ready);

module_platform_driver(mtk_musb_driver);

MODULE_DESCRIPTION("MediaTek MUSB Glue Layer");
MODULE_AUTHOR("Min Guo <min.guo@mediatek.com>");
MODULE_LICENSE("GPL v2");
