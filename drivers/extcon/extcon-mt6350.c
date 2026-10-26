// SPDX-License-Identifier: GPL-2.0-only
/* MT6350 physical USB VBUS detection and system wakeup. */
#include <linux/extcon-provider.h>
#include <linux/interrupt.h>
#include <linux/mfd/mt6350/registers.h>
#include <linux/mfd/mt6350/usb.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/property.h>
#include <linux/regmap.h>

#define MT6350_CHR_CON0		0x0000
#define MT6350_PCHR_CHRDET	BIT(4)
#define MT6350_USB_WAKE_MS	5000

struct mt6350_usb {
	struct device *dev;
	struct regmap *regmap;
	struct extcon_dev *edev;
	struct mutex lock;
	int irq, error;
	u32 events, con0, status;
	bool valid, online, wake_armed;
};

static const unsigned int mt6350_usb_cables[] = { EXTCON_USB, EXTCON_NONE };

static int mt6350_usb_sample(struct mt6350_usb *usb, bool irq_event)
{
	unsigned int con0, status;
	bool online;
	int ret;

	mutex_lock(&usb->lock);
	if (irq_event)
		usb->events++;
	ret = regmap_read(usb->regmap, MT6350_CHR_CON0, &con0);
	if (ret)
		goto out;
	ret = regmap_read(usb->regmap, MT6350_CHRSTATUS, &status);
	if (ret)
		goto out;
	usb->con0 = con0;
	usb->status = status;
	online = !!(status & MT6350_PCHR_CHRDET);
	if (!usb->valid || online != usb->online) {
		ret = extcon_set_state_sync(usb->edev, EXTCON_USB, online);
		if (ret)
			goto out;
		WRITE_ONCE(usb->online, online);
		WRITE_ONCE(usb->valid, true);
	}
out:
	WRITE_ONCE(usb->error, ret);
	mutex_unlock(&usb->lock);
	return ret;
}

static irqreturn_t mt6350_usb_irq(int irq, void *data)
{
	struct mt6350_usb *usb = data;
	int ret;

	/* Allow charger status and the USB host's enumeration to complete. */
	pm_wakeup_event(usb->dev, MT6350_USB_WAKE_MS);
	ret = mt6350_usb_sample(usb, true);
	if (ret)
		dev_err_ratelimited(usb->dev, "VBUS read failed: %d\n", ret);
	return IRQ_HANDLED;
}

static ssize_t state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct mt6350_usb *usb = dev_get_drvdata(dev);
	ssize_t len;

	/* Cached snapshot; do not access a suspended supplier from sysfs. */
	mutex_lock(&usb->lock);
	len = sysfs_emit(buf, "irq=%d events=%u online=%u valid=%u error=%d wake_armed=%u\n"
			"con0=%04x chrstatus=%04x\n",
			usb->irq, usb->events, usb->online, usb->valid, usb->error,
			usb->wake_armed, usb->con0, usb->status);
	mutex_unlock(&usb->lock);
	return len;
}
static DEVICE_ATTR_RO(state);

static struct attribute *mt6350_usb_attrs[] = { &dev_attr_state.attr, NULL };
ATTRIBUTE_GROUPS(mt6350_usb);

static void mt6350_usb_wakeup_cleanup(void *data)
{
	struct mt6350_usb *usb = data;

	if (usb->wake_armed)
		disable_irq_wake(usb->irq);
	device_init_wakeup(usb->dev, false);
}

static int mt6350_usb_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6397_chip *chip = dev_get_drvdata(dev->parent);
	struct mt6350_usb *usb;
	int ret;

	if (!chip || chip->chip_id != MT6350_CHIP_ID)
		return -ENODEV;
	usb = devm_kzalloc(dev, sizeof(*usb), GFP_KERNEL);
	if (!usb)
		return -ENOMEM;
	usb->dev = dev;
	usb->regmap = chip->regmap;
	mutex_init(&usb->lock);
	usb->irq = platform_get_irq_byname(pdev, "charger-detect");
	if (usb->irq < 0)
		return usb->irq;
	usb->edev = devm_extcon_dev_allocate(dev, mt6350_usb_cables);
	if (IS_ERR(usb->edev))
		return PTR_ERR(usb->edev);
	ret = devm_extcon_dev_register(dev, usb->edev);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, usb);
	ret = mt6350_usb_sample(usb, false);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot read VBUS status\n");
	ret = devm_request_threaded_irq(dev, usb->irq, NULL, mt6350_usb_irq,
					IRQF_ONESHOT, dev_name(dev), usb);
	if (ret)
		return ret;
	ret = device_init_wakeup(dev, device_property_read_bool(dev, "wakeup-source"));
	if (ret)
		return ret;
	return devm_add_action_or_reset(dev, mt6350_usb_wakeup_cleanup, usb);
}

static int mt6350_usb_suspend(struct device *dev)
{
	struct mt6350_usb *usb = dev_get_drvdata(dev);
	int ret;

	ret = mt6350_usb_sample(usb, false);
	if (ret)
		return ret;
	if (!device_may_wakeup(dev))
		return 0;
	/* Arm before the MFD parent's late callback installs its wake mask. */
	ret = enable_irq_wake(usb->irq);
	if (!ret)
		WRITE_ONCE(usb->wake_armed, true);
	return ret;
}

static int mt6350_usb_resume(struct device *dev)
{
	struct mt6350_usb *usb = dev_get_drvdata(dev);
	int ret;

	if (usb->wake_armed) {
		ret = disable_irq_wake(usb->irq);
		if (ret)
			return ret;
		WRITE_ONCE(usb->wake_armed, false);
	}
	return mt6350_usb_sample(usb, false);
}

static DEFINE_SIMPLE_DEV_PM_OPS(mt6350_usb_pm_ops, mt6350_usb_suspend, mt6350_usb_resume);

static const struct of_device_id mt6350_usb_of_match[] = {
	{ .compatible = "mediatek,mt6350-usb" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6350_usb_of_match);

static struct platform_driver mt6350_usb_driver = {
	.probe = mt6350_usb_probe,
	.driver = {
		.name = "mt6350-usb",
		.of_match_table = mt6350_usb_of_match,
		.pm = pm_sleep_ptr(&mt6350_usb_pm_ops),
		.dev_groups = mt6350_usb_groups,
	},
};

bool mt6350_usb_suspend_ready(struct device *dev)
{
	struct mt6350_usb *usb;

	if (dev->driver != &mt6350_usb_driver.driver || !device_may_wakeup(dev))
		return false;
	usb = dev_get_drvdata(dev);
	return READ_ONCE(usb->valid) && !READ_ONCE(usb->online) &&
		!READ_ONCE(usb->error) && READ_ONCE(usb->wake_armed);
}
EXPORT_SYMBOL_GPL(mt6350_usb_suspend_ready);

module_platform_driver(mt6350_usb_driver);

MODULE_DESCRIPTION("MT6350 USB VBUS detection and wakeup");
MODULE_LICENSE("GPL");
