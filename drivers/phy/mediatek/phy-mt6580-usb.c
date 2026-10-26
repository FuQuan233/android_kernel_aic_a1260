// SPDX-License-Identifier: GPL-2.0-only
/*
 * MT6580 device-mode USB2 PHY.
 * Register sequence from MediaTek's 2015 GPLv2 usb20/mt6580/usb20_phy.c.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>

struct mt6580_usb_phy {
	void __iomem *base;
	struct clk *ref;
};

static void mt6580_usb_update(struct mt6580_usb_phy *priv, u32 offset,
			      u8 clear, u8 set)
{
	u8 value = readb(priv->base + offset);

	writeb((value & ~clear) | set, priv->base + offset);
}

static void mt6580_usb_calibrate(struct phy *phy)
{
	struct mt6580_usb_phy *priv = phy_get_drvdata(phy);
	u32 value, measured;
	u32 slew = 4;
	int ret;

	/* PHY base = USB_SIF + 0x800; frequency meter = USB_SIF + 0xf00. */
	writeb(0x80, priv->base + 0x15);
	udelay(1);
	writeb(0x01, priv->base + 0x711);
	writeb(0x04, priv->base + 0x701);
	mt6580_usb_update(priv, 0x703, 0, BIT(0));
	ret = readb_poll_timeout(priv->base + 0x710, value, value & BIT(0),
				10, 10000);
	measured = readl(priv->base + 0x70c);
	if (!ret && measured)
		slew = min_t(u32, DIV_ROUND_CLOSEST(1024U * 48 * 28 / measured,
						  1000), 7);
	else
		dev_warn(&phy->dev, "Slew calibration unavailable; using vendor default 4\n");
	mt6580_usb_update(priv, 0x703, BIT(0), 0);
	mt6580_usb_update(priv, 0x711, BIT(0), 0);
	writeb(slew << 4, priv->base + 0x15);
}

static int mt6580_usb_power_on(struct phy *phy)
{
	struct mt6580_usb_phy *priv = phy_get_drvdata(phy);
	int ret;

	ret = clk_prepare_enable(priv->ref);
	if (ret)
		return ret;
	usleep_range(50, 100);
	/* Leave UART/GPIO/charger-detection modes and release forced UTMI signals. */
	mt6580_usb_update(priv, 0x1d, 0x10, 0);
	mt6580_usb_update(priv, 0x6b, 0x04, 0);
	mt6580_usb_update(priv, 0x6e, 0x01, 0);
	mt6580_usb_update(priv, 0x21, 0x03, 0);
	mt6580_usb_update(priv, 0x68, 0xf4, 0);
	mt6580_usb_update(priv, 0x69, 0x3c, 0);
	mt6580_usb_update(priv, 0x6a, 0xbe, 0);
	mt6580_usb_update(priv, 0x1a, 0x80, 0x10);
	mt6580_usb_update(priv, 0x22, 0x03, 0x04);
	mt6580_usb_update(priv, 0x18, 0x08, 0x06);
	usleep_range(800, 1000);
	/* Force device-mode VBUS/session validity, as in the vendor BSP. */
	mt6580_usb_update(priv, 0x6c, 0x10, 0x2f);
	mt6580_usb_update(priv, 0x6d, 0, 0x3f);
	mt6580_usb_calibrate(phy);

	return 0;
}

static int mt6580_usb_power_off(struct phy *phy)
{
	struct mt6580_usb_phy *priv = phy_get_drvdata(phy);

	/* BSP savecurrent: settle forced UTMI signals before removing 48 MHz.
	 * Clearing VBUS validity alone leaves the analog PHY running.
	 */
	mt6580_usb_update(priv, 0x6b, 0x04, 0);
	mt6580_usb_update(priv, 0x6e, 0x01, 0);
	mt6580_usb_update(priv, 0x21, 0x03, 0);
	mt6580_usb_update(priv, 0x6a, 0, 0x04);
	mt6580_usb_update(priv, 0x68, 0x30, 0xdc);
	mt6580_usb_update(priv, 0x69, 0x3c, 0);
	mt6580_usb_update(priv, 0x6a, 0, 0xba);
	mt6580_usb_update(priv, 0x1a, 0x90, 0);
	usleep_range(800, 1000);
	mt6580_usb_update(priv, 0x68, 0x08, 0);
	udelay(2);
	mt6580_usb_update(priv, 0x6c, 0x2e, 0x10);
	clk_disable_unprepare(priv->ref);
	return 0;
}

static int mt6580_usb_set_mode(struct phy *phy, enum phy_mode mode, int submode)
{
	return mode == PHY_MODE_USB_DEVICE ? 0 : -EOPNOTSUPP;
}

static const struct phy_ops mt6580_usb_phy_ops = {
	.power_on = mt6580_usb_power_on,
	.power_off = mt6580_usb_power_off,
	.set_mode = mt6580_usb_set_mode,
	.owner = THIS_MODULE,
};

static int mt6580_usb_phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6580_usb_phy *priv;
	struct phy_provider *provider;
	struct phy *phy;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);
	priv->ref = devm_clk_get(dev, "ref");
	if (IS_ERR(priv->ref))
		return dev_err_probe(dev, PTR_ERR(priv->ref), "Missing 48 MHz PHY clock\n");
	phy = devm_phy_create(dev, dev->of_node, &mt6580_usb_phy_ops);
	if (IS_ERR(phy))
		return PTR_ERR(phy);
	phy_set_drvdata(phy, priv);
	provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	dev_info(dev, "Device-only PHY; PHY core manages the VUSB supply\n");
	return PTR_ERR_OR_ZERO(provider);
}

static const struct of_device_id mt6580_usb_phy_match[] = {
	{ .compatible = "mediatek,mt6580-usbphy" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6580_usb_phy_match);

static struct platform_driver mt6580_usb_phy_driver = {
	.probe = mt6580_usb_phy_probe,
	.driver = {
		.name = "mt6580-usbphy",
		.of_match_table = mt6580_usb_phy_match,
	},
};
module_platform_driver(mt6580_usb_phy_driver);

MODULE_DESCRIPTION("MT6580 device-mode USB2 PHY");
MODULE_LICENSE("GPL");
