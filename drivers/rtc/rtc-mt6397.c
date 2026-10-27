// SPDX-License-Identifier: GPL-2.0-only
/*
* Copyright (c) 2014-2015 MediaTek Inc.
* Author: Tianping.Fang <tianping.fang@mediatek.com>
*/

#include <linux/err.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nvmem-provider.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reboot.h>
#include <linux/rtc.h>
#include <linux/mfd/mt6397/rtc.h>
#include <linux/mod_devicetable.h>

#define MT6350_RTC_AL_HOU		0x001c
#define MT6350_RTC_FGSOC_MASK	GENMASK(14, 8)
#define MT6350_RTC_FGSOC_SHIFT	8

static int mtk_rtc_write_trigger(struct mt6397_rtc *rtc)
{
	int ret;
	u32 data;

	ret = regmap_write(rtc->regmap, rtc->addr_base + rtc->data->wrtgr, 1);
	if (ret < 0)
		return ret;

	ret = regmap_read_poll_timeout(rtc->regmap,
					rtc->addr_base + RTC_BBPU, data,
					!(data & RTC_BBPU_CBUSY),
					MTK_RTC_POLL_DELAY_US,
					MTK_RTC_POLL_TIMEOUT);
	if (ret < 0)
		dev_err(rtc->rtc_dev->dev.parent,
			"failed to write WRTGR: %d\n", ret);

	return ret;
}

static int mt6350_rtc_soc_read(void *priv, unsigned int offset,
			       void *val, size_t bytes)
{
	struct mt6397_rtc *rtc = priv;
	unsigned int data;
	int ret;

	if (offset || bytes != 1)
		return -EINVAL;

	mutex_lock(&rtc->lock);
	ret = regmap_read(rtc->regmap, rtc->addr_base + MT6350_RTC_AL_HOU,
			  &data);
	if (!ret)
		*(u8 *)val = (data & MT6350_RTC_FGSOC_MASK) >>
			    MT6350_RTC_FGSOC_SHIFT;
	mutex_unlock(&rtc->lock);

	return ret;
}

static int mt6350_rtc_soc_write(void *priv, unsigned int offset,
				void *val, size_t bytes)
{
	struct mt6397_rtc *rtc = priv;
	unsigned int readback;
	u8 soc;
	int ret;

	if (offset || bytes != 1)
		return -EINVAL;
	soc = *(u8 *)val;
	if (soc > 100)
		return -EINVAL;

	/* The factory gauge shares AL_HOU with the alarm hour. Serialize the
	 * masked write and commit with all RTC time/alarm operations; zero
	 * remains available to invalidate a saved state.
	 */
	mutex_lock(&rtc->lock);
	ret = regmap_update_bits(rtc->regmap,
				 rtc->addr_base + MT6350_RTC_AL_HOU,
				 MT6350_RTC_FGSOC_MASK,
				 soc << MT6350_RTC_FGSOC_SHIFT);
	if (!ret)
		ret = mtk_rtc_write_trigger(rtc);
	if (!ret)
		ret = regmap_read(rtc->regmap, rtc->addr_base + MT6350_RTC_AL_HOU,
				  &readback);
	if (!ret && ((readback & MT6350_RTC_FGSOC_MASK) >>
		     MT6350_RTC_FGSOC_SHIFT) != soc)
		ret = -EIO;
	mutex_unlock(&rtc->lock);

	return ret;
}

static int mt6350_rtc_soc_register(struct mt6397_rtc *rtc)
{
	struct nvmem_config config = {
		.name = "mt6350-rtc-soc",
		.id = NVMEM_DEVID_AUTO,
		.type = NVMEM_TYPE_BATTERY_BACKED,
		.root_only = true,
		.size = 1,
		.word_size = 1,
		.stride = 1,
		.reg_read = mt6350_rtc_soc_read,
		.reg_write = mt6350_rtc_soc_write,
		.priv = rtc,
	};

	return devm_rtc_nvmem_register(rtc->rtc_dev, &config);
}

/* MT6350's MMIO PWRAP regmap uses a spinlock and atomic polling. */
static int mt6350_rtc_poweroff_write(struct mt6397_rtc *rtc, u32 reg, u32 val)
{
	unsigned int busy;
	int ret;

	ret = regmap_write(rtc->regmap, rtc->addr_base + reg, val);
	if (ret)
		return ret;
	ret = regmap_write(rtc->regmap, rtc->addr_base + rtc->data->wrtgr, 1);
	if (ret)
		return ret;

	return regmap_read_poll_timeout_atomic(rtc->regmap,
			rtc->addr_base + RTC_BBPU, busy,
			!(busy & RTC_BBPU_CBUSY), 10, 100000);
}

static int mt6350_rtc_poweroff(struct sys_off_data *data)
{
	struct mt6397_rtc *rtc = data->cb_data;
	unsigned int users;
	int ret;

	/* Match the MT6350 BSP: unlock, preserve 32 kHz users, drop BBPU.
	 * This is the final atomic callback, after devices and other CPUs stop;
	 * do not take rtc->lock or use the sleeping RTC trigger helper here.
	 */
	ret = mt6350_rtc_poweroff_write(rtc, 0x36, 0x586a);
	if (ret)
		goto fail;
	ret = mt6350_rtc_poweroff_write(rtc, 0x36, 0x9136);
	if (ret)
		goto fail;
	ret = regmap_read(rtc->regmap, rtc->addr_base + 0x2c, &users);
	if (ret)
		goto fail;
	if (!(users & GENMASK(12, 8))) {
		unsigned int con;

		ret = regmap_read(rtc->regmap, rtc->addr_base + 0x3e, &con);
		if (ret)
			goto fail;
		/* F32KOB disables the unused RTC clock output. */
		ret = mt6350_rtc_poweroff_write(rtc, 0x3e, con | BIT(5));
		if (ret)
			goto fail;
	}

	pr_emerg("MT6350: dropping RTC BBPU power hold\n");
	/* PWREN | AUTO; BBPU bit 2 cleared. Keep RTC alarm power-on possible. */
	ret = mt6350_rtc_poweroff_write(rtc, RTC_BBPU,
				       RTC_BBPU_KEY | BIT(0) | BIT(3));
	if (!ret) {
		mdelay(1000);
		ret = -ETIMEDOUT;
	}
fail:
	pr_emerg("MT6350: power-off did not complete: %d\n", ret);
	return NOTIFY_DONE;
}

static irqreturn_t mtk_rtc_irq_handler_thread(int irq, void *data)
{
	struct mt6397_rtc *rtc = data;
	u32 irqsta, irqen;
	int ret;

	ret = regmap_read(rtc->regmap, rtc->addr_base + RTC_IRQ_STA, &irqsta);
	if ((ret >= 0) && (irqsta & RTC_IRQ_STA_AL)) {
		rtc_update_irq(rtc->rtc_dev, 1, RTC_IRQF | RTC_AF);
		irqen = irqsta & ~RTC_IRQ_EN_AL;
		mutex_lock(&rtc->lock);
		if (regmap_write(rtc->regmap, rtc->addr_base + RTC_IRQ_EN,
				 irqen) == 0)
			mtk_rtc_write_trigger(rtc);
		mutex_unlock(&rtc->lock);

		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}

static int __mtk_rtc_read_time(struct mt6397_rtc *rtc,
			       struct rtc_time *tm, int *sec)
{
	int ret;
	u16 data[RTC_OFFSET_COUNT];

	mutex_lock(&rtc->lock);
	ret = regmap_bulk_read(rtc->regmap, rtc->addr_base + RTC_TC_SEC,
			       data, RTC_OFFSET_COUNT);
	if (ret < 0)
		goto exit;

	tm->tm_sec = data[RTC_OFFSET_SEC];
	tm->tm_min = data[RTC_OFFSET_MIN];
	tm->tm_hour = data[RTC_OFFSET_HOUR];
	tm->tm_mday = data[RTC_OFFSET_DOM];
	tm->tm_wday = data[RTC_OFFSET_DOW];
	tm->tm_mon = data[RTC_OFFSET_MTH] & RTC_TC_MTH_MASK;
	tm->tm_year = data[RTC_OFFSET_YEAR] + rtc->data->year_offset;

	ret = regmap_read(rtc->regmap, rtc->addr_base + RTC_TC_SEC, sec);
exit:
	mutex_unlock(&rtc->lock);
	return ret;
}

static int mtk_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct mt6397_rtc *rtc = dev_get_drvdata(dev);
	int sec, ret;

	do {
		ret = __mtk_rtc_read_time(rtc, tm, &sec);
		if (ret < 0)
			goto exit;
	} while (sec < tm->tm_sec);

	/* HW register start mon/wday from one, but tm_mon/tm_wday start from zero. */
	tm->tm_mon--;
	tm->tm_wday--;

exit:
	return ret;
}

static int mtk_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct mt6397_rtc *rtc = dev_get_drvdata(dev);
	int ret;
	u16 data[RTC_OFFSET_COUNT];

	tm->tm_mon++;
	tm->tm_wday++;

	data[RTC_OFFSET_SEC] = tm->tm_sec;
	data[RTC_OFFSET_MIN] = tm->tm_min;
	data[RTC_OFFSET_HOUR] = tm->tm_hour;
	data[RTC_OFFSET_DOM] = tm->tm_mday;
	data[RTC_OFFSET_DOW] = tm->tm_wday;
	data[RTC_OFFSET_MTH] = tm->tm_mon;
	data[RTC_OFFSET_YEAR] = tm->tm_year - rtc->data->year_offset;

	mutex_lock(&rtc->lock);
	ret = regmap_bulk_write(rtc->regmap, rtc->addr_base + RTC_TC_SEC,
				data, RTC_OFFSET_COUNT);
	if (ret < 0)
		goto exit;

	/* Time register write to hardware after call trigger function */
	ret = mtk_rtc_write_trigger(rtc);

exit:
	mutex_unlock(&rtc->lock);
	return ret;
}

static int mtk_rtc_read_alarm(struct device *dev, struct rtc_wkalrm *alm)
{
	struct rtc_time *tm = &alm->time;
	struct mt6397_rtc *rtc = dev_get_drvdata(dev);
	u32 irqen, pdn2;
	int ret;
	u16 data[RTC_OFFSET_COUNT];

	mutex_lock(&rtc->lock);
	ret = regmap_read(rtc->regmap, rtc->addr_base + RTC_IRQ_EN, &irqen);
	if (ret < 0)
		goto err_exit;
	ret = regmap_read(rtc->regmap, rtc->addr_base + RTC_PDN2, &pdn2);
	if (ret < 0)
		goto err_exit;

	ret = regmap_bulk_read(rtc->regmap, rtc->addr_base + RTC_AL_SEC,
			       data, RTC_OFFSET_COUNT);
	if (ret < 0)
		goto err_exit;

	alm->enabled = !!(irqen & RTC_IRQ_EN_AL);
	alm->pending = !!(pdn2 & RTC_PDN2_PWRON_ALARM);
	mutex_unlock(&rtc->lock);

	tm->tm_sec = data[RTC_OFFSET_SEC] & RTC_AL_SEC_MASK;
	tm->tm_min = data[RTC_OFFSET_MIN] & RTC_AL_MIN_MASK;
	tm->tm_hour = data[RTC_OFFSET_HOUR] & RTC_AL_HOU_MASK;
	tm->tm_mday = data[RTC_OFFSET_DOM] & RTC_AL_DOM_MASK;
	tm->tm_mon = data[RTC_OFFSET_MTH] & RTC_AL_MTH_MASK;
	tm->tm_year = (data[RTC_OFFSET_YEAR] & RTC_AL_YEA_MASK) + rtc->data->year_offset;

	tm->tm_mon--;

	return 0;
err_exit:
	mutex_unlock(&rtc->lock);
	return ret;
}

static int mtk_rtc_set_alarm(struct device *dev, struct rtc_wkalrm *alm)
{
	struct rtc_time *tm = &alm->time;
	struct mt6397_rtc *rtc = dev_get_drvdata(dev);
	int ret;
	u16 data[RTC_OFFSET_COUNT];

	tm->tm_mon++;

	mutex_lock(&rtc->lock);
	ret = regmap_bulk_read(rtc->regmap, rtc->addr_base + RTC_AL_SEC,
			       data, RTC_OFFSET_COUNT);
	if (ret < 0)
		goto exit;

	data[RTC_OFFSET_SEC] = ((data[RTC_OFFSET_SEC] & ~(RTC_AL_SEC_MASK)) |
				(tm->tm_sec & RTC_AL_SEC_MASK));
	data[RTC_OFFSET_MIN] = ((data[RTC_OFFSET_MIN] & ~(RTC_AL_MIN_MASK)) |
				(tm->tm_min & RTC_AL_MIN_MASK));
	data[RTC_OFFSET_HOUR] = ((data[RTC_OFFSET_HOUR] & ~(RTC_AL_HOU_MASK)) |
				(tm->tm_hour & RTC_AL_HOU_MASK));
	data[RTC_OFFSET_DOM] = ((data[RTC_OFFSET_DOM] & ~(RTC_AL_DOM_MASK)) |
				(tm->tm_mday & RTC_AL_DOM_MASK));
	data[RTC_OFFSET_MTH] = ((data[RTC_OFFSET_MTH] & ~(RTC_AL_MTH_MASK)) |
				(tm->tm_mon & RTC_AL_MTH_MASK));
	data[RTC_OFFSET_YEAR] = ((data[RTC_OFFSET_YEAR] & ~(RTC_AL_YEA_MASK)) |
				((tm->tm_year - rtc->data->year_offset) & RTC_AL_YEA_MASK));

	if (alm->enabled) {
		ret = regmap_bulk_write(rtc->regmap,
					rtc->addr_base + RTC_AL_SEC,
					data, RTC_OFFSET_COUNT);
		if (ret < 0)
			goto exit;
		ret = regmap_write(rtc->regmap, rtc->addr_base + RTC_AL_MASK,
				   RTC_AL_MASK_DOW);
		if (ret < 0)
			goto exit;
		ret = regmap_update_bits(rtc->regmap,
					 rtc->addr_base + RTC_IRQ_EN,
					 RTC_IRQ_EN_ONESHOT_AL,
					 RTC_IRQ_EN_ONESHOT_AL);
		if (ret < 0)
			goto exit;
	} else {
		ret = regmap_update_bits(rtc->regmap,
					 rtc->addr_base + RTC_IRQ_EN,
					 RTC_IRQ_EN_ONESHOT_AL, 0);
		if (ret < 0)
			goto exit;
	}

	/* All alarm time register write to hardware after calling
	 * mtk_rtc_write_trigger. This can avoid race condition if alarm
	 * occur happen during writing alarm time register.
	 */
	ret = mtk_rtc_write_trigger(rtc);
exit:
	mutex_unlock(&rtc->lock);
	return ret;
}

static const struct rtc_class_ops mtk_rtc_ops = {
	.read_time  = mtk_rtc_read_time,
	.set_time   = mtk_rtc_set_time,
	.read_alarm = mtk_rtc_read_alarm,
	.set_alarm  = mtk_rtc_set_alarm,
};

static int mtk_rtc_probe(struct platform_device *pdev)
{
	struct resource *res;
	struct mt6397_chip *mt6397_chip = dev_get_drvdata(pdev->dev.parent);
	struct mt6397_rtc *rtc;
	int ret;

	rtc = devm_kzalloc(&pdev->dev, sizeof(struct mt6397_rtc), GFP_KERNEL);
	if (!rtc)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -EINVAL;
	rtc->addr_base = res->start;

	rtc->data = of_device_get_match_data(&pdev->dev);

	rtc->irq = platform_get_irq(pdev, 0);
	if (rtc->irq < 0)
		return rtc->irq;

	rtc->regmap = mt6397_chip->regmap;
	mutex_init(&rtc->lock);

	platform_set_drvdata(pdev, rtc);

	rtc->rtc_dev = devm_rtc_allocate_device(&pdev->dev);
	if (IS_ERR(rtc->rtc_dev))
		return PTR_ERR(rtc->rtc_dev);

	/* MT6350's boot firmware may leave the RTC write interface locked. */
	if (rtc->data->unlock) {
		ret = regmap_write(rtc->regmap, rtc->addr_base + 0x36, 0x586a);
		if (!ret)
			ret = mtk_rtc_write_trigger(rtc);
		if (!ret)
			ret = regmap_write(rtc->regmap, rtc->addr_base + 0x36, 0x9136);
		if (!ret)
			ret = mtk_rtc_write_trigger(rtc);
		if (ret)
			return dev_err_probe(&pdev->dev, ret, "RTC write unlock failed\n");
	}

	ret = devm_request_threaded_irq(&pdev->dev, rtc->irq, NULL,
					mtk_rtc_irq_handler_thread,
					IRQF_ONESHOT | IRQF_TRIGGER_HIGH,
					"mt6397-rtc", rtc);

	if (ret) {
		dev_err(&pdev->dev, "Failed to request alarm IRQ: %d: %d\n",
			rtc->irq, ret);
		return ret;
	}

	device_init_wakeup(&pdev->dev, true);

	rtc->rtc_dev->ops = &mtk_rtc_ops;
	rtc->rtc_dev->range_min = RTC_TIMESTAMP_BEGIN_1900;
	rtc->rtc_dev->range_max = mktime64(2027, 12, 31, 23, 59, 59);
	rtc->rtc_dev->start_secs = mktime64(1968, 1, 2, 0, 0, 0);
	rtc->rtc_dev->set_start_time = true;
	if (rtc->data->year_offset) {
		/* MT6350 stores year zero as 1968, matching its factory BSP. */
		rtc->rtc_dev->range_min = mktime64(1968, 1, 1, 0, 0, 0);
		rtc->rtc_dev->range_max = mktime64(2095, 12, 31, 23, 59, 59);
		rtc->rtc_dev->set_start_time = false;
	}

	ret = devm_rtc_register_device(rtc->rtc_dev);
	if (ret)
		return ret;
	if (of_device_is_compatible(pdev->dev.of_node, "mediatek,mt6350-rtc")) {
		ret = mt6350_rtc_soc_register(rtc);
		if (ret)
			return dev_err_probe(&pdev->dev, ret,
					     "Failed to register RTC SOC storage\n");
	}
	if (rtc->data->poweroff)
		return devm_register_sys_off_handler(&pdev->dev,
			SYS_OFF_MODE_POWER_OFF, SYS_OFF_PRIO_DEFAULT,
			mt6350_rtc_poweroff, rtc);
	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int mt6397_rtc_suspend(struct device *dev)
{
	struct mt6397_rtc *rtc = dev_get_drvdata(dev);

	if (device_may_wakeup(dev))
		enable_irq_wake(rtc->irq);

	return 0;
}

static int mt6397_rtc_resume(struct device *dev)
{
	struct mt6397_rtc *rtc = dev_get_drvdata(dev);

	if (device_may_wakeup(dev))
		disable_irq_wake(rtc->irq);

	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(mt6397_pm_ops, mt6397_rtc_suspend,
			mt6397_rtc_resume);

static const struct mtk_rtc_data mt6358_rtc_data = {
	.wrtgr = RTC_WRTGR_MT6358,
};

static const struct mtk_rtc_data mt6397_rtc_data = {
	.wrtgr = RTC_WRTGR_MT6397,
};

static const struct mtk_rtc_data mt6350_rtc_data = {
	.wrtgr = RTC_WRTGR_MT6397,
	.unlock = true,
	.poweroff = true,
	.year_offset = 68,
};

static const struct of_device_id mt6397_rtc_of_match[] = {
	{ .compatible = "mediatek,mt6323-rtc", .data = &mt6397_rtc_data },
	{ .compatible = "mediatek,mt6350-rtc", .data = &mt6350_rtc_data },
	{ .compatible = "mediatek,mt6357-rtc", .data = &mt6358_rtc_data },
	{ .compatible = "mediatek,mt6358-rtc", .data = &mt6358_rtc_data },
	{ .compatible = "mediatek,mt6397-rtc", .data = &mt6397_rtc_data },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6397_rtc_of_match);

static struct platform_driver mtk_rtc_driver = {
	.driver = {
		.name = "mt6397-rtc",
		.of_match_table = mt6397_rtc_of_match,
		.pm = &mt6397_pm_ops,
	},
	.probe	= mtk_rtc_probe,
};

module_platform_driver(mtk_rtc_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Tianping Fang <tianping.fang@mediatek.com>");
MODULE_DESCRIPTION("RTC Driver for MediaTek MT6397 PMIC");
