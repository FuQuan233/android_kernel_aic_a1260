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
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
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
	s64 last_sample_ms;
	int online;
	int capacity;
	int seed_uv;
	bool seed_from_hw;
	int full_ms;
	int empty_ms;
	bool model_ready;
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

static int mt6350_battery_update_model(struct mt6350_battery *bat)
{
	union power_supply_propval online, status, limit;
	s64 now = ktime_to_ms(ktime_get());
	s64 elapsed = now - bat->last_sample_ms;
	unsigned int raw;
	int dt = 0, uv, ret;

	ret = power_supply_get_property(bat->charger, POWER_SUPPLY_PROP_ONLINE, &online);
	if (ret)
		return ret;
	ret = power_supply_get_property(bat->charger, POWER_SUPPLY_PROP_STATUS, &status);
	if (ret)
		return ret;
	ret = power_supply_get_property(bat->charger,
			POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT, &limit);
	if (ret)
		return ret;
	limit.intval = clamp(limit.intval, 0, bat->info->constant_charge_current_max_ua);
	if (status.intval != POWER_SUPPLY_STATUS_CHARGING)
		limit.intval = 0;

	if (!bat->present) {
		bat->model_ready = false;
		bat->last_sample_ms = 0;
		bat->full_ms = bat->empty_ms = 0;
		bat->capacity = 0;
		return 0;
	}
	if (!bat->model_ready) {
		bat->seed_uv = bat->voltage_uv;
		bat->seed_from_hw = false;
		/* External charger board: BSP's WAKEUP_SWCHR (ADC9), captured
		 * before the live charging/load voltage, plus its 8 mV trim.
		 * Only seed once. This is not a continuously refreshed OCV.
		 */
		ret = regmap_read(bat->regmap, MT6350_AUXADC_ADC9, &raw);
		if (!ret && (raw & BIT(15))) {
			uv = div_u64((u64)(raw & GENMASK(14, 0)) * 7200000, 32768) + 8000;
			if (uv >= bat->info->voltage_min_design_uv &&
			    uv <= bat->info->voltage_max_design_uv &&
			    abs(uv - bat->voltage_uv) <= 300000) {
				bat->seed_uv = uv;
				bat->seed_from_hw = true;
			}
		}
		mt6350_model_init(&bat->model, bat->info, bat->seed_uv, bat->temp);
		bat->model_ready = true;
		dev_info(bat->dev, "SW gauge seed=%duV (%s), voltage=%duV\n",
			 bat->seed_uv, bat->seed_from_hw ? "startup OCV" : "terminal fallback",
			 bat->voltage_uv);
	} else if (bat->last_sample_ms && elapsed > 0 && elapsed <= 30000 &&
		   bat->online == online.intval) {
		/* Never integrate a post-plug voltage over the preceding interval,
		 * nor extrapolate an active current through unobserved long gaps.
		 */
		dt = elapsed;
	}
	bat->last_sample_ms = now;
	bat->online = online.intval;
	mt6350_model_step(&bat->model, bat->info, bat->voltage_uv, bat->temp,
			 dt, online.intval, limit.intval);
	if (!dt)
		bat->full_ms = bat->empty_ms = 0;

	/* Anchor only after sustained full/low-voltage observations. A status
	 * transition alone must not jump to 100%, or hide a critically low cell.
	 */
	if (online.intval && status.intval == POWER_SUPPLY_STATUS_FULL &&
	    bat->voltage_uv >= bat->info->constant_charge_voltage_max_uv - 100000)
		bat->full_ms = min(bat->full_ms + dt, 60000);
	else
		bat->full_ms = 0;
	if (!online.intval && bat->voltage_uv <= bat->info->voltage_min_design_uv)
		bat->empty_ms = min(bat->empty_ms + dt, 30000);
	else
		bat->empty_ms = 0;
	if (bat->full_ms == 60000) {
		bat->model.charge_uas = bat->model.full_uas;
		bat->model.remainder = 0;
	} else if (bat->empty_ms == 30000) {
		bat->model.charge_uas = 0;
		bat->model.remainder = 0;
	}
	bat->capacity = DIV_ROUND_CLOSEST(mt6350_model_ppm(&bat->model), 10000);
	if (bat->full_ms != 60000)
		bat->capacity = min(bat->capacity, 99);
	return 0;
}

static void mt6350_battery_work(struct work_struct *work)
{
	struct mt6350_battery *bat = container_of(to_delayed_work(work),
					struct mt6350_battery, work);

	mutex_lock(&bat->lock);
	bat->error = mt6350_battery_sample(bat);
	if (!bat->error)
		bat->error = mt6350_battery_update_model(bat);
	if (bat->error)
		bat->last_sample_ms = 0;
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
		"initialized=%u seed_uv=%d seed_hw=%u voltage_uv=%d model_ocv_uv=%d "
		"resistance_mohm=%d estimated_discharge_ua=%d soc_ppm=%d capacity=%d "
		"online=%d full_ms=%d empty_ms=%d error=%d\n",
		bat->model_ready, bat->seed_uv, bat->seed_from_hw, bat->voltage_uv,
		bat->model.ocv_uv, bat->model.resistance_mohm, bat->model.current_ua,
		bat->model_ready ? mt6350_model_ppm(&bat->model) : 0, bat->capacity,
		bat->online, bat->full_ms, bat->empty_ms, bat->error);
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

static int mt6350_battery_resume(struct device *dev)
{
	struct mt6350_battery *bat = dev_get_drvdata(dev);

	mutex_lock(&bat->lock);
	/* No measured sleep current: do not integrate the awake load over sleep. */
	bat->last_sample_ms = 0;
	bat->full_ms = bat->empty_ms = 0;
	if (bat->ready)
		mod_delayed_work(system_freezable_wq, &bat->work, 0);
	mutex_unlock(&bat->lock);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(mt6350_battery_pm_ops, NULL, mt6350_battery_resume);

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
	if (!bat->info->ocv_table[0] || bat->info->charge_full_design_uah <= 0 ||
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
	dev_info(bat->dev, "AUXADC battery monitor; OCV/resistance software gauge\n");
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
