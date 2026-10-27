// SPDX-License-Identifier: GPL-2.0-only
/* MT6350 AUXADC battery monitoring, using the MT6580 BSP conversion factors. */
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/mfd/mt6350/registers.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#include "mt6350-battery-model.h"

struct mt6350_battery {
	struct device *dev;
	struct regmap *regmap;
	struct power_supply *psy;
	struct power_supply *charger;
	struct power_supply_battery_info *info;
	struct delayed_work work;
	struct mutex lock;
	int voltage_uv;
	int temp;
	int present;
	int error;
	bool ready;
	struct mt6350_battery_model model;
	struct nvmem_cell *soc_cell;
	time64_t last_sample_sec;
	time64_t last_ui_sec;
	s64 suspend_offset_ns;
	u64 sleep_total_sec;
	int suspend_ocv_mv;
	int last_sleep_sec;
	int resume_ocv_mv;
	unsigned int sleep_ocv_resets;
	int online;
	int restore_charge_type;
	int capacity;
	int filtered_soc;
	int seed_uv;
	int rtc_soc;
	int rtc_error;
	unsigned int rtc_writes;
	unsigned int sync_count;
	bool seed_from_hw;
	bool seed_from_rtc;
	bool ever_full;
	bool full_reset_done;
	bool model_ready;
	bool suspended;
	bool warm_boot;
};

/* Same 10 kohm NTC, 16.9 kohm pull-up / 27 kohm pull-down as the BSP. */
static const int mt6350_ntc_resistance[] = {
	68237, 53650, 42506, 33892, 27219, 22021, 17926, 14674, 12081,
	10000, 8315, 6948, 5834, 4917, 4161, 3535, 3014,
};

static int mt6350_battery_temp(int uv, int *temp)
{
	s64 parallel, resistance;
	int i;

	if (uv <= 0 || uv >= 1800000)
		return -ERANGE;
	parallel = div_s64((s64)16900 * uv, 1800000 - uv);
	if (parallel >= 27000)
		return -ERANGE;
	resistance = div_s64(parallel * 27000, 27000 - parallel);
	if (resistance >= mt6350_ntc_resistance[0]) {
		*temp = -200;
		return 0;
	}
	for (i = 1; i < ARRAY_SIZE(mt6350_ntc_resistance); i++) {
		int high = mt6350_ntc_resistance[i - 1];
		int low = mt6350_ntc_resistance[i];

		if (resistance >= low) {
			*temp = -200 + (i - 1) * 50 +
				div_s64((high - resistance) * 50, high - low);
			return 0;
		}
	}
	*temp = 600;
	return 0;
}

static int mt6350_battery_adc(struct mt6350_battery *bat, int channel,
			      unsigned int reg, int ratio, int *uv)
{
	unsigned int value;
	int ret;

	ret = regmap_update_bits(bat->regmap, MT6350_AUXADC_CON22, BIT(channel), 0);
	if (ret)
		return ret;
	ret = regmap_update_bits(bat->regmap, MT6350_AUXADC_CON22,
				 BIT(channel), BIT(channel));
	if (ret)
		return ret;
	usleep_range(300, 500);
	ret = regmap_read_poll_timeout(bat->regmap, reg, value,
				      value & BIT(15), 1000, 30000);
	if (ret)
		return ret;
	*uv = div_u64((u64)(value & GENMASK(14, 0)) * ratio * 1800000, 32768);
	return 0;
}

/* Only this driver requests AUXADC channels; serialize conversions and cache. */
static int mt6350_battery_sample(struct mt6350_battery *bat)
{
	unsigned int status;
	int voltage, thermistor, temp, ret, i, sum = 0;

	for (i = 0; i < 5; i++) {
		ret = mt6350_battery_adc(bat, 7, MT6350_AUXADC_ADC0, 4, &voltage);
		if (ret)
			return ret;
		sum += voltage;
	}
	voltage = sum / 5;
	if (voltage < 2000000 || voltage > 5000000)
		return -ERANGE;
	ret = mt6350_battery_adc(bat, 5, MT6350_AUXADC_ADC3, 1, &thermistor);
	if (ret)
		return ret;
	ret = mt6350_battery_temp(thermistor, &temp);
	if (ret)
		return ret;
	ret = regmap_read(bat->regmap, 0x000e, &status);
	if (ret)
		return ret;
	bat->voltage_uv = voltage;
	bat->temp = temp;
	bat->present = !(status & BIT(12));
	return 0;
}

static int mt6350_battery_hw_ocv(struct mt6350_battery *bat, int *uv)
{
	unsigned int raw;
	int ret;

	/* External-charger MT6350 uses WAKEUP_SWCHR (ADC9), plus 8 mV trim. */
	ret = regmap_read(bat->regmap, MT6350_AUXADC_ADC9, &raw);
	if (ret)
		return ret;
	if (!(raw & BIT(15)))
		return -ENODATA;
	*uv = div_u64((u64)(raw & GENMASK(14, 0)) * 7200000, 32768) + 8000;
	return *uv >= 2000000 && *uv <= 5000000 ? 0 : -ERANGE;
}

static void mt6350_battery_save_soc(struct mt6350_battery *bat)
{
	u8 soc = max(bat->capacity, 1);
	int ret;

	/* Factory RTC persistence stores UI SOC, with zero reserved as invalid. */
	if (!bat->soc_cell || soc == bat->rtc_soc)
		return;
	ret = nvmem_cell_write(bat->soc_cell, &soc, sizeof(soc));
	bat->rtc_error = ret == sizeof(soc) ? 0 : (ret < 0 ? ret : -EIO);
	if (bat->rtc_error) {
		dev_warn_ratelimited(bat->dev, "Cannot save RTC SOC: %d\n",
				     bat->rtc_error);
		return;
	}
	bat->rtc_soc = soc;
	bat->rtc_writes++;
}

static void mt6350_battery_read_soc(struct mt6350_battery *bat)
{
	size_t len;
	u8 *data;

	if (!bat->soc_cell)
		return;
	data = nvmem_cell_read(bat->soc_cell, &len);
	if (IS_ERR(data)) {
		bat->rtc_error = PTR_ERR(data);
		dev_warn(bat->dev, "Cannot read RTC SOC: %d\n", bat->rtc_error);
		return;
	}
	if (len == 1 && data[0] <= 100) {
		bat->rtc_soc = data[0];
		bat->rtc_error = 0;
	} else {
		bat->rtc_error = -EINVAL;
	}
	kfree(data);
}

static int mt6350_battery_restore_charger(struct mt6350_battery *bat)
{
	union power_supply_propval type = { .intval = bat->restore_charge_type };
	int ret;

	if (!type.intval)
		return 0;
	ret = power_supply_set_property(bat->charger,
				POWER_SUPPLY_PROP_CHARGE_TYPE, &type);
	if (!ret)
		bat->restore_charge_type = 0;
	return ret;
}

static int mt6350_battery_seed(struct mt6350_battery *bat, bool online)
{
	union power_supply_propval charge_type, off = {
		.intval = POWER_SUPPLY_CHARGE_TYPE_NONE,
	};
	int seed_soc, vbat_soc, uv, ret;
	bool paused = false;

	/* The original oam_init pauses charging for 50 ms before comparing
	 * retained hardware OCV and averaged terminal voltage. Restore the
	 * exact charger mode through its power_supply API on every exit.
	 */
	if (online) {
		ret = power_supply_get_property(bat->charger,
				POWER_SUPPLY_PROP_CHARGE_TYPE, &charge_type);
		if (ret)
			return ret;
		if (charge_type.intval == POWER_SUPPLY_CHARGE_TYPE_FAST ||
		    charge_type.intval == POWER_SUPPLY_CHARGE_TYPE_TRICKLE) {
			bat->restore_charge_type = charge_type.intval;
			ret = power_supply_set_property(bat->charger,
					POWER_SUPPLY_PROP_CHARGE_TYPE, &off);
			if (ret) {
				mt6350_battery_restore_charger(bat);
				return ret;
			}
			paused = true;
		}
	}
	msleep(50);
	ret = mt6350_battery_sample(bat);
	if (paused) {
		int restore = mt6350_battery_restore_charger(bat);

		if (restore)
			return restore;
	}
	if (ret)
		return ret;

	bat->seed_uv = bat->voltage_uv;
	bat->seed_from_hw = !mt6350_battery_hw_ocv(bat, &uv);
	if (bat->seed_from_hw)
		bat->seed_uv = uv;
	mt6350_model_init(&bat->model, bat->seed_uv / 1000, bat->temp / 10, -1);
	seed_soc = mt6350_model_soc(&bat->model);
	vbat_soc = mt6350_model_soc_by_voltage(&bat->model, bat->voltage_uv / 1000);
	/* Factory power-on tolerances: 90%, 30% and RTC delta < 40%. */
	if (online && ((seed_soc == 100 && vbat_soc < 90) ||
		       abs(seed_soc - vbat_soc) > 30)) {
		bat->seed_uv = bat->voltage_uv;
		bat->seed_from_hw = false;
		seed_soc = vbat_soc;
	}
	bat->seed_from_rtc = bat->rtc_soc > 0 &&
		((abs(bat->rtc_soc - seed_soc) < 40 && (seed_soc > 5 || online)) ||
		 bat->warm_boot);
	if (bat->seed_from_rtc)
		seed_soc = bat->rtc_soc;
	if (!seed_soc && online)
		seed_soc = 1;
	mt6350_model_init(&bat->model, bat->seed_uv / 1000, bat->temp / 10, seed_soc);
	bat->capacity = seed_soc;
	bat->filtered_soc = seed_soc;
	bat->model_ready = true;
	dev_info(bat->dev, "Factory OAM seed=%duV hw=%u rtc=%u SOC=%d Qmax=%dmAh\n",
		 bat->seed_uv, bat->seed_from_hw, bat->seed_from_rtc,
		 seed_soc, bat->model.qmax_mah);
	return 0;
}

static void mt6350_battery_update_ui(struct mt6350_battery *bat, int status,
				     bool charger_event)
{
	int soc = bat->filtered_soc;

	/* Factory low-voltage tracking: drop one UI point per battery tick;
	 * at <=3.4 V it may reach zero, above that preserve the last 1%.
	 */
	if (bat->voltage_uv <= 3450000) {
		if (bat->capacity > 0 &&
		    (bat->voltage_uv <= 3400000 || bat->capacity != 1))
			bat->capacity--;
		mt6350_model_reset(&bat->model, bat->capacity);
		return;
	}

	if (bat->online && status == POWER_SUPPLY_STATUS_FULL) {
		if (bat->capacity < 100) {
			bat->capacity++;
			mt6350_model_reset(&bat->model, bat->capacity);
		} else if (!bat->full_reset_done) {
			mt6350_model_reset(&bat->model, 100);
			bat->full_reset_done = true;
		}
		if (bat->capacity == 100)
			bat->ever_full = true;
		if (bat->ever_full && bat->capacity - soc > 10)
			bat->ever_full = false;
		else
			return;
	} else {
		bat->full_reset_done = false;
		if (bat->online && !bat->ever_full)
			bat->capacity = min(bat->capacity, 99);
	}

	/* The original counter waits six 10-second periods, and applies the
	 * decrement on the following tick. A cable event skips that update.
	 */
	if (bat->capacity > soc && bat->capacity != 1) {
		if (!charger_event) {
			if (bat->sync_count == 6) {
				bat->capacity--;
				bat->sync_count = 0;
			} else {
				bat->sync_count++;
			}
		}
	} else {
		bat->sync_count = 0;
		bat->capacity = soc;
	}
	bat->capacity = max(bat->capacity, 1);
	/* Legacy charging could infer BATFULL from UI=100. The mainline BQ
	 * supplies real charge completion; retain that independent authority.
	 */
	if (bat->online && !bat->ever_full && status != POWER_SUPPLY_STATUS_FULL)
		bat->capacity = min(bat->capacity, 99);
}

static int mt6350_battery_update_model(struct mt6350_battery *bat)
{
	union power_supply_propval online, status;
	time64_t now = ktime_get_seconds();
	time64_t ui_now = ktime_get_boottime_seconds();
	int dt = 0, ret;
	bool cable_event;

	ret = power_supply_get_property(bat->charger, POWER_SUPPLY_PROP_ONLINE, &online);
	if (ret)
		return ret;
	ret = power_supply_get_property(bat->charger, POWER_SUPPLY_PROP_STATUS, &status);
	if (ret)
		return ret;
	cable_event = bat->model_ready && bat->online != online.intval;
	bat->online = online.intval;
	if (!bat->present) {
		bat->model_ready = false;
		bat->last_sample_sec = 0;
		bat->capacity = 0;
		return 0;
	}
	if (!bat->model_ready) {
		ret = mt6350_battery_seed(bat, online.intval);
		if (ret)
			return ret;
		bat->last_ui_sec = ui_now;
	} else if (bat->last_sample_sec && now > bat->last_sample_sec) {
		dt = min_t(time64_t, now - bat->last_sample_sec, INT_MAX);
	}
	bat->last_sample_sec = now;
	mt6350_model_step(&bat->model, bat->voltage_uv / 1000, bat->temp / 10, dt);
	/* Factory battery_common filters rebound-induced rises while offline. */
	if (bat->online)
		bat->filtered_soc = mt6350_model_soc(&bat->model);
	else
		bat->filtered_soc = min(bat->filtered_soc, mt6350_model_soc(&bat->model));
	if (ui_now - bat->last_ui_sec >= 10) {
		mt6350_battery_update_ui(bat, status.intval, cable_event);
		bat->last_ui_sec = ui_now;
	}
	mt6350_battery_save_soc(bat);
	return 0;
}

static void mt6350_battery_work(struct work_struct *work)
{
	struct mt6350_battery *bat = container_of(to_delayed_work(work),
					struct mt6350_battery, work);

	mutex_lock(&bat->lock);
	bat->error = mt6350_battery_restore_charger(bat);
	if (!bat->error)
		bat->error = mt6350_battery_sample(bat);
	if (!bat->error)
		bat->error = mt6350_battery_update_model(bat);
	if (bat->error)
		bat->last_sample_sec = 0;
	if (bat->ready)
		queue_delayed_work(system_freezable_wq, &bat->work, msecs_to_jiffies(10000));
	mutex_unlock(&bat->lock);
	if (bat->error)
		dev_err_ratelimited(bat->dev, "Battery sample failed: %d\n", bat->error);
	power_supply_changed(bat->psy);
}

static int mt6350_battery_get_property(struct power_supply *psy,
				     enum power_supply_property prop,
				     union power_supply_propval *val)
{
	struct mt6350_battery *bat = power_supply_get_drvdata(psy);
	int ret = 0;

	mutex_lock(&bat->lock);
	if (bat->error) {
		ret = bat->error;
		goto out;
	}
	switch (prop) {
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = bat->present;
		break;
	case POWER_SUPPLY_PROP_STATUS:
	case POWER_SUPPLY_PROP_HEALTH:
		ret = power_supply_get_property(bat->charger, prop, val);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = bat->voltage_uv;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		val->intval = bat->temp;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = bat->capacity;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = bat->info->technology;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = bat->info->charge_full_design_uah;
		break;
	default:
		ret = -EINVAL;
	}
out:
	mutex_unlock(&bat->lock);
	return ret;
}

static const enum power_supply_property mt6350_battery_properties[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
};

static void mt6350_battery_external_power_changed(struct power_supply *psy)
{
	struct mt6350_battery *bat = power_supply_get_drvdata(psy);

	mutex_lock(&bat->lock);
	if (bat->ready)
		mod_delayed_work(system_freezable_wq, &bat->work, 0);
	mutex_unlock(&bat->lock);
}

static const struct power_supply_desc mt6350_battery_desc = {
	.name = "mt6350-battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = mt6350_battery_properties,
	.num_properties = ARRAY_SIZE(mt6350_battery_properties),
	.get_property = mt6350_battery_get_property,
	.external_power_changed = mt6350_battery_external_power_changed,
};

static void mt6350_battery_free_info(void *data)
{
	struct mt6350_battery *bat = data;

	power_supply_put_battery_info(bat->psy, bat->info);
}

static void mt6350_battery_stop(void *data)
{
	struct mt6350_battery *bat = data;

	mutex_lock(&bat->lock);
	bat->ready = false;
	if (mt6350_battery_restore_charger(bat))
		dev_warn(bat->dev, "Cannot restore charger while stopping gauge\n");
	mutex_unlock(&bat->lock);
	cancel_delayed_work_sync(&bat->work);
}

static ssize_t gauge_state_show(struct device *dev, struct device_attribute *attr,
			       char *buf)
{
	struct mt6350_battery *bat = dev_get_drvdata(dev);
	int len;

	mutex_lock(&bat->lock);
	len = sysfs_emit(buf,
		"initialized=%u seed_uv=%d seed_hw=%u seed_rtc=%u voltage_uv=%d "
		"ocv1_mv=%d ocv2_mv=%d r1_mohm=%d r2_mohm=%d "
		"estimated_i1_01ma=%d estimated_i2_01ma=%d car1_01mah=%lld "
		"car2_01mah=%lld dod0=%d dod1=%d dod2=%d dod3=%d qmax_mah=%d "
		"capacity=%d filtered_soc=%d rtc_soc=%d rtc_error=%d rtc_writes=%u warm_boot=%u "
		"last_sleep_sec=%d sleep_total_sec=%llu sleep_ocv_before_mv=%d "
		"sleep_ocv_after_mv=%d sleep_ocv_resets=%u online=%d error=%d\n",
		bat->model_ready, bat->seed_uv, bat->seed_from_hw, bat->seed_from_rtc,
		bat->voltage_uv, bat->model.ocv1_mv, bat->model.ocv2_mv,
		bat->model.r1_mohm, bat->model.r2_mohm, bat->model.current1_01ma,
		bat->model.current2_01ma, bat->model.car1_01mah, bat->model.car2_01mah,
		bat->model.dod0, bat->model.dod1, bat->model.dod2, bat->model.dod3,
		bat->model.qmax_mah, bat->capacity, bat->filtered_soc, bat->rtc_soc, bat->rtc_error,
		bat->rtc_writes, bat->warm_boot, bat->last_sleep_sec, bat->sleep_total_sec,
		bat->suspend_ocv_mv, bat->resume_ocv_mv, bat->sleep_ocv_resets,
		bat->online, bat->error);
	mutex_unlock(&bat->lock);
	return len;
}
static DEVICE_ATTR_RO(gauge_state);
static struct attribute *mt6350_battery_attrs[] = {
	&dev_attr_gauge_state.attr,
	NULL,
};
static const struct attribute_group mt6350_battery_group = {
	.attrs = mt6350_battery_attrs,
};

static int mt6350_battery_suspend(struct device *dev)
{
	struct mt6350_battery *bat = dev_get_drvdata(dev);
	int uv;

	mutex_lock(&bat->lock);
	bat->suspend_ocv_mv = 0;
	if (!mt6350_battery_hw_ocv(bat, &uv))
		bat->suspend_ocv_mv = uv / 1000;
	bat->suspend_offset_ns = (s64)ktime_get_boottime_ns() - ktime_get_ns();
	bat->suspended = true;
	mutex_unlock(&bat->lock);
	return 0;
}

static int mt6350_battery_resume(struct device *dev)
{
	struct mt6350_battery *bat = dev_get_drvdata(dev);
	s64 offset = (s64)ktime_get_boottime_ns() - ktime_get_ns();
	unsigned int charger;
	int uv, new_ocv_mv, sleep_sec;
	bool online;

	mutex_lock(&bat->lock);
	/* Subtract awake time around device callbacks; failed suspend must not
	 * be counted as battery sleep. Awake integration uses MONOTONIC.
	 */
	if (bat->suspended && offset > bat->suspend_offset_ns && bat->model_ready) {
		sleep_sec = min_t(s64, div_s64(offset - bat->suspend_offset_ns,
					    NSEC_PER_SEC), INT_MAX);
		new_ocv_mv = bat->suspend_ocv_mv;
		if (!mt6350_battery_hw_ocv(bat, &uv))
			new_ocv_mv = uv / 1000;
		online = bat->online;
		if (!regmap_read(bat->regmap, MT6350_CHRSTATUS, &charger))
			online = !!(charger & BIT(4));
		if (sleep_sec) {
			bat->resume_ocv_mv = new_ocv_mv;
			if (sleep_sec > 1800 && !online &&
			    new_ocv_mv < bat->suspend_ocv_mv)
				bat->sleep_ocv_resets++;
			mt6350_model_sleep(&bat->model, sleep_sec,
				bat->suspend_ocv_mv, new_ocv_mv, online);
			bat->last_sleep_sec = sleep_sec;
			bat->sleep_total_sec += sleep_sec;
		}
	}
	bat->suspended = false;
	if (bat->ready)
		mod_delayed_work(system_freezable_wq, &bat->work, 0);
	mutex_unlock(&bat->lock);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(mt6350_battery_pm_ops,
		mt6350_battery_suspend, mt6350_battery_resume);

/* Retain the factory warm-reset/recovery override when LK supplies its
 * original tags. Missing boot information never forces a stale RTC SOC.
 */
static bool mt6350_battery_warm_boot(void)
{
	struct device_node *chosen = of_find_node_by_path("/chosen");
	const char *args, *reason;
	const void *tag;
	int len, code;
	bool warm = false;

	if (!chosen)
		return false;
	if (!of_property_read_string(chosen, "bootargs", &args)) {
		reason = strstr(args, "boot_reason=");
		if (reason && (reason == args || reason[-1] == ' ') &&
		    sscanf(reason, "boot_reason=%d", &code) == 1)
			warm = code >= 3 && code <= 6;
	}
	tag = of_get_property(chosen, "atag,boot", &len);
	if (tag && len >= 12 && get_unaligned_le32(tag + 8) == 2)
		warm = true;
	of_node_put(chosen);
	return warm;
}

static int mt6350_battery_probe(struct platform_device *pdev)
{
	struct mt6397_chip *chip = dev_get_drvdata(pdev->dev.parent);
	struct power_supply_config config = {};
	struct mt6350_battery *bat;
	int ret;

	bat = devm_kzalloc(&pdev->dev, sizeof(*bat), GFP_KERNEL);
	if (!bat)
		return -ENOMEM;
	bat->dev = &pdev->dev;
	bat->regmap = chip->regmap;
	platform_set_drvdata(pdev, bat);
	bat->error = -EAGAIN;
	mutex_init(&bat->lock);
	INIT_DELAYED_WORK(&bat->work, mt6350_battery_work);
	bat->soc_cell = devm_nvmem_cell_get(bat->dev, "soc");
	if (IS_ERR(bat->soc_cell)) {
		ret = PTR_ERR(bat->soc_cell);
		if (ret != -ENOENT)
			return dev_err_probe(bat->dev, ret, "RTC SOC unavailable\n");
		bat->soc_cell = NULL;
	}
	mt6350_battery_read_soc(bat);
	bat->warm_boot = mt6350_battery_warm_boot();
	bat->charger = devm_power_supply_get_by_reference(bat->dev, "power-supplies");
	if (IS_ERR(bat->charger))
		return dev_err_probe(bat->dev, PTR_ERR(bat->charger), "Charger unavailable\n");
	if (!bat->charger)
		return -EPROBE_DEFER;

	/* Enable the ADC clocks/buffer and the BSP BATON presence detector. */
	ret = regmap_update_bits(bat->regmap, MT6350_TOP_CKPDN1, BIT(15), 0);
	if (ret)
		return ret;
	ret = regmap_update_bits(bat->regmap, MT6350_TOP_CKPDN2,
				 GENMASK(6, 4), 0);
	if (ret)
		return ret;
	ret = regmap_update_bits(bat->regmap, MT6350_AUXADC_CON11, BIT(4), BIT(4));
	if (ret)
		return ret;
	ret = regmap_update_bits(bat->regmap, 0x000e, BIT(2) | BIT(0), BIT(2) | BIT(0));
	if (ret)
		return ret;

	config.drv_data = bat;
	config.fwnode = dev_fwnode(&pdev->dev);
	bat->psy = devm_power_supply_register(bat->dev, &mt6350_battery_desc, &config);
	if (IS_ERR(bat->psy))
		return PTR_ERR(bat->psy);
	ret = power_supply_get_battery_info(bat->psy, &bat->info);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(bat->dev, mt6350_battery_free_info, bat);
	if (ret)
		return ret;
	if (bat->info->charge_full_design_uah <= 0 ||
	    bat->info->charge_full_design_uah > INT_MAX / 2 ||
	    bat->info->constant_charge_current_max_ua <= 0 ||
	    bat->info->constant_charge_voltage_max_uv <= 0 ||
	    bat->info->voltage_min_design_uv <= 0)
		return -EINVAL;
	ret = devm_device_add_group(bat->dev, &mt6350_battery_group);
	if (ret)
		return ret;
	/* Register cancellation last, before dependent resources are released. */
	ret = devm_add_action_or_reset(bat->dev, mt6350_battery_stop, bat);
	if (ret)
		return ret;
	bat->ready = true;
	queue_delayed_work(system_freezable_wq, &bat->work, 0);
	dev_info(bat->dev, "AUXADC battery monitor; factory OAM dual-observer gauge\n");
	return 0;
}

static const struct of_device_id mt6350_battery_match[] = {
	{ .compatible = "mediatek,mt6350-battery" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6350_battery_match);

static struct platform_driver mt6350_battery_driver = {
	.probe = mt6350_battery_probe,
	.driver = {
		.name = "mt6350-battery",
		.of_match_table = mt6350_battery_match,
		.pm = pm_sleep_ptr(&mt6350_battery_pm_ops),
	},
};
module_platform_driver(mt6350_battery_driver);
MODULE_DESCRIPTION("MT6350 AUXADC battery monitor");
MODULE_LICENSE("GPL");
