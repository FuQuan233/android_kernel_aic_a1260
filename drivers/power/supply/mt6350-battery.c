// SPDX-License-Identifier: GPL-2.0-only
/* MT6350 AUXADC battery monitoring, using the MT6580 BSP conversion factors. */
#include <linux/delay.h>
#include <linux/iopoll.h>
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
	int voltage, thermistor, temp, ret;

	ret = mt6350_battery_adc(bat, 7, MT6350_AUXADC_ADC0, 4, &voltage);
	if (ret)
		return ret;
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

static void mt6350_battery_work(struct work_struct *work)
{
	struct mt6350_battery *bat = container_of(to_delayed_work(work),
					struct mt6350_battery, work);

	mutex_lock(&bat->lock);
	bat->error = mt6350_battery_sample(bat);
	if (bat->ready)
		queue_delayed_work(system_freezable_wq, &bat->work, msecs_to_jiffies(10000));
	mutex_unlock(&bat->lock);
	if (bat->error)
		dev_err_ratelimited(bat->dev, "Battery ADC sample failed: %d\n", bat->error);
	power_supply_changed(bat->psy);
}

static int mt6350_battery_get_property(struct power_supply *psy,
				     enum power_supply_property prop,
				     union power_supply_propval *val)
{
	struct mt6350_battery *bat = power_supply_get_drvdata(psy);
	union power_supply_propval status;
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
		val->intval = power_supply_batinfo_ocv2cap(bat->info,
						bat->voltage_uv, bat->temp / 10);
		ret = power_supply_get_property(bat->charger, POWER_SUPPLY_PROP_STATUS,
					       &status);
		if (!ret && status.intval == POWER_SUPPLY_STATUS_FULL)
			val->intval = 100;
		else
			val->intval = clamp(val->intval, 0, 99);
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
	if (!bat->info->ocv_table[0] || bat->info->charge_full_design_uah <= 0)
		return -EINVAL;
	/* Register cancellation last, before dependent resources are released. */
	ret = devm_add_action_or_reset(bat->dev, mt6350_battery_stop, bat);
	if (ret)
		return ret;
	bat->ready = true;
	queue_delayed_work(system_freezable_wq, &bat->work, 0);
	dev_info(bat->dev, "AUXADC battery monitor; voltage-based capacity estimate\n");
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
	},
};
module_platform_driver(mt6350_battery_driver);
MODULE_DESCRIPTION("MT6350 AUXADC battery monitor");
MODULE_LICENSE("GPL");
