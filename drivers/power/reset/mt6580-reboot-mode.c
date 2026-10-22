// SPDX-License-Identifier: GPL-2.0-only
/* MT6580/MT6350 LK fastboot marker, using the firmware-initialized wrapper. */
#include <linux/bitfield.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>

#define WACS2_CMD	0x9c
#define WACS2_RDATA	0xa0
#define WACS2_VLDCLR	0xa4
#define WACS_FSM	GENMASK(18, 16)
#define WACS_IDLE	0
#define WACS_VALID	6
#define WACS_INIT_DONE	BIT(21)
#define RTC_BBPU	0x8000
#define RTC_PDN1	0x802c
#define RTC_PROT	0x8036
#define RTC_WRTGR	0x803c
#define RTC_BUSY	BIT(6)
#define RTC_FAST_BOOT	BIT(13)

struct mt6580_reboot {
	void __iomem *base;
	struct device *dev;
	struct mutex lock;
	struct notifier_block notifier;
	bool prepared;
};

static int mt6580_pmic_xfer(struct mt6580_reboot *rb, u16 reg, u16 *value, bool write)
{
	u32 status;
	int ret;

	status = readl(rb->base + WACS2_RDATA);
	if (!(status & WACS_INIT_DONE))
		return -ENODEV;
	/* Finish an abandoned read before issuing another transaction. */
	if (FIELD_GET(WACS_FSM, status) == WACS_VALID)
		writel(1, rb->base + WACS2_VLDCLR);
	ret = readl_poll_timeout(rb->base + WACS2_RDATA, status,
				FIELD_GET(WACS_FSM, status) == WACS_IDLE, 10, 10000);
	if (ret)
		return ret;
	writel((write ? BIT(31) : 0) | ((u32)(reg >> 1) << 16) |
	       (write ? *value : 0), rb->base + WACS2_CMD);
	if (write)
		return readl_poll_timeout(rb->base + WACS2_RDATA, status,
			FIELD_GET(WACS_FSM, status) == WACS_IDLE, 10, 10000);
	ret = readl_poll_timeout(rb->base + WACS2_RDATA, status,
				FIELD_GET(WACS_FSM, status) == WACS_VALID, 10, 10000);
	if (ret)
		return ret;
	*value = status & 0xffff;
	writel(1, rb->base + WACS2_VLDCLR);
	return 0;
}

static int mt6580_rtc_commit(struct mt6580_reboot *rb, u16 reg, u16 value)
{
	u16 trigger = 1, busy;
	unsigned int tries;
	int ret;

	ret = mt6580_pmic_xfer(rb, reg, &value, true);
	if (ret)
		return ret;
	ret = mt6580_pmic_xfer(rb, RTC_WRTGR, &trigger, true);
	if (ret)
		return ret;
	for (tries = 0; tries < 100; tries++) {
		ret = mt6580_pmic_xfer(rb, RTC_BBPU, &busy, false);
		if (ret || !(busy & RTC_BUSY))
			return ret;
		usleep_range(1000, 1100);
	}
	return -ETIMEDOUT;
}

static int mt6580_prepare_bootloader(struct mt6580_reboot *rb)
{
	u16 pdn1, check;
	int ret;

	mutex_lock(&rb->lock);
	if (rb->prepared) {
		ret = 0;
		goto out;
	}
	/* Vendor rtc_writeif_unlock, followed by rtc_mark_fast. */
	ret = mt6580_rtc_commit(rb, RTC_PROT, 0x586a);
	if (ret)
		goto out;
	ret = mt6580_rtc_commit(rb, RTC_PROT, 0x9136);
	if (ret)
		goto out;
	ret = mt6580_pmic_xfer(rb, RTC_PDN1, &pdn1, false);
	if (ret)
		goto out;
	ret = mt6580_rtc_commit(rb, RTC_PDN1, pdn1 | RTC_FAST_BOOT);
	if (ret)
		goto out;
	ret = mt6580_pmic_xfer(rb, RTC_PDN1, &check, false);
	if (!ret && check != (pdn1 | RTC_FAST_BOOT))
		ret = -EIO;
	if (!ret) {
		rb->prepared = true;
		dev_info(rb->dev, "LK fastboot marker verified: RTC_PDN1=%04x\n", check);
	}
out:
	mutex_unlock(&rb->lock);
	return ret;
}

static ssize_t bootloader_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	int ret;

	if (!sysfs_streq(buf, "1"))
		return -EINVAL;
	ret = mt6580_prepare_bootloader(dev_get_drvdata(dev));
	return ret ? ret : count;
}
static DEVICE_ATTR_WO(bootloader);
static struct attribute *mt6580_reboot_attrs[] = { &dev_attr_bootloader.attr, NULL };
ATTRIBUTE_GROUPS(mt6580_reboot);

static int mt6580_reboot_notify(struct notifier_block *nb, unsigned long action, void *cmd)
{
	struct mt6580_reboot *rb = container_of(nb, struct mt6580_reboot, notifier);
	int ret;

	if (action != SYS_RESTART || !cmd || strcmp(cmd, "bootloader"))
		return NOTIFY_DONE;
	ret = mt6580_prepare_bootloader(rb);
	if (ret)
		dev_err(rb->dev, "cannot set LK fastboot marker: %d\n", ret);
	return NOTIFY_DONE;
}

static int mt6580_reboot_probe(struct platform_device *pdev)
{
	struct mt6580_reboot *rb;

	rb = devm_kzalloc(&pdev->dev, sizeof(*rb), GFP_KERNEL);
	if (!rb)
		return -ENOMEM;
	rb->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(rb->base))
		return PTR_ERR(rb->base);
	rb->dev = &pdev->dev;
	mutex_init(&rb->lock);
	platform_set_drvdata(pdev, rb);
	rb->notifier.notifier_call = mt6580_reboot_notify;
	return devm_register_reboot_notifier(&pdev->dev, &rb->notifier);
}

static const struct of_device_id mt6580_reboot_match[] = {
	{ .compatible = "wingtech,wt6580-reboot-mode" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6580_reboot_match);

static struct platform_driver mt6580_reboot_driver = {
	.probe = mt6580_reboot_probe,
	.driver = {
		.name = "mt6580-reboot-mode",
		.of_match_table = mt6580_reboot_match,
		.dev_groups = mt6580_reboot_groups,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(mt6580_reboot_driver);
MODULE_DESCRIPTION("WT6580 LK fastboot RTC marker");
MODULE_LICENSE("GPL");
