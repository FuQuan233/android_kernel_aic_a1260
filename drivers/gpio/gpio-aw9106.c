// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Awinic AW9106 GPIO controller.
 * Register definitions follow AWINIC's GPL AW9106 driver and AW9106C datasheet.
 * GPIO offsets match the OUT0..OUT5 package pin names, not register bank order.
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#define AW9106_INPUT		0x00
#define AW9106_OUTPUT		0x02
#define AW9106_DIRECTION		0x04
#define AW9106_IRQ_MASK		0x06
#define AW9106_ID		0x10
#define AW9106_GPIO_MODE		0x12
#define AW9106_CHIP_ID		0x23
#define AW9106_NGPIO		6
#define AW9106_GPIO_MASK		GENMASK(5, 0)

struct aw9106 {
	struct device *dev;
	struct regmap *regmap;
	struct gpio_chip gpio;
	struct mutex lock;
	u8 irq_mask;
	u8 input_state;
};

static unsigned int aw9106_bank(unsigned int offset)
{
	return offset < 4 ? 1 : 0;
}

static unsigned int aw9106_bit(unsigned int offset)
{
	return BIT(offset < 4 ? offset : offset - 4);
}

static unsigned int aw9106_bank_mask(unsigned int bank)
{
	return bank ? GENMASK(3, 0) : GENMASK(1, 0);
}

static unsigned int aw9106_bank_bits(u8 bits, unsigned int bank)
{
	return bank ? bits & GENMASK(3, 0) : (bits >> 4) & GENMASK(1, 0);
}

/* The registers must be read separately to acknowledge both IRQ banks. */
static int aw9106_read_inputs(struct aw9106 *awi, u8 *state)
{
	unsigned int p0, p1;
	int ret;

	ret = regmap_read(awi->regmap, AW9106_INPUT, &p0);
	if (ret)
		return ret;
	ret = regmap_read(awi->regmap, AW9106_INPUT + 1, &p1);
	if (ret)
		return ret;

	*state = ((p0 & GENMASK(1, 0)) << 4) | (p1 & GENMASK(3, 0));
	return 0;
}

static int aw9106_gpio_request(struct gpio_chip *gc, unsigned int offset)
{
	struct aw9106 *awi = gpiochip_get_data(gc);
	unsigned int bit = aw9106_bit(offset);
	unsigned int bank = aw9106_bank(offset);
	int ret;

	/* Release the pin before selecting GPIO, avoiding a reset-default output. */
	mutex_lock(&awi->lock);
	ret = regmap_update_bits(awi->regmap, AW9106_DIRECTION + bank, bit, bit);
	if (!ret)
		ret = regmap_update_bits(awi->regmap, AW9106_GPIO_MODE + bank,
					 bit, bit);
	mutex_unlock(&awi->lock);
	return ret;
}

static int aw9106_gpio_get_direction(struct gpio_chip *gc, unsigned int offset)
{
	struct aw9106 *awi = gpiochip_get_data(gc);
	unsigned int val;
	int ret;

	ret = regmap_read(awi->regmap, AW9106_DIRECTION + aw9106_bank(offset),
			  &val);
	if (ret)
		return ret;

	return !!(val & aw9106_bit(offset));
}

static int aw9106_gpio_direction_input(struct gpio_chip *gc, unsigned int offset)
{
	struct aw9106 *awi = gpiochip_get_data(gc);
	unsigned int bit = aw9106_bit(offset);

	return regmap_update_bits(awi->regmap,
				  AW9106_DIRECTION + aw9106_bank(offset), bit, bit);
}

static int aw9106_gpio_direction_output(struct gpio_chip *gc,
				       unsigned int offset, int value)
{
	struct aw9106 *awi = gpiochip_get_data(gc);
	unsigned int bank = aw9106_bank(offset);
	unsigned int bit = aw9106_bit(offset);
	int ret;

	mutex_lock(&awi->lock);
	/* Set the latch before enabling output, to avoid an unwanted pulse. */
	ret = regmap_update_bits(awi->regmap, AW9106_OUTPUT + bank, bit,
				 value ? bit : 0);
	if (!ret)
		ret = regmap_update_bits(awi->regmap, AW9106_DIRECTION + bank,
					 bit, 0);
	mutex_unlock(&awi->lock);

	return ret;
}

static int aw9106_gpio_get(struct gpio_chip *gc, unsigned int offset)
{
	struct aw9106 *awi = gpiochip_get_data(gc);
	unsigned int val;
	int ret;

	ret = regmap_read(awi->regmap, AW9106_INPUT + aw9106_bank(offset), &val);
	if (ret)
		return ret;

	return !!(val & aw9106_bit(offset));
}

static int aw9106_gpio_set(struct gpio_chip *gc, unsigned int offset, int value)
{
	struct aw9106 *awi = gpiochip_get_data(gc);
	unsigned int bit = aw9106_bit(offset);

	return regmap_update_bits(awi->regmap,
				  AW9106_OUTPUT + aw9106_bank(offset), bit,
				  value ? bit : 0);
}

static void aw9106_irq_mask(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct aw9106 *awi = gpiochip_get_data(gc);

	awi->irq_mask |= BIT(irqd_to_hwirq(d));
	gpiochip_disable_irq(gc, irqd_to_hwirq(d));
}

static void aw9106_irq_unmask(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct aw9106 *awi = gpiochip_get_data(gc);

	gpiochip_enable_irq(gc, irqd_to_hwirq(d));
	awi->irq_mask &= ~BIT(irqd_to_hwirq(d));
}

static int aw9106_irq_set_type(struct irq_data *d, unsigned int type)
{
	/* The chip signals every input transition; it has no polarity selector. */
	return type == IRQ_TYPE_NONE || type == IRQ_TYPE_EDGE_BOTH ? 0 : -EINVAL;
}

static void aw9106_irq_bus_lock(struct irq_data *d)
{
	struct aw9106 *awi = gpiochip_get_data(irq_data_get_irq_chip_data(d));

	mutex_lock(&awi->lock);
}

static void aw9106_irq_bus_sync_unlock(struct irq_data *d)
{
	struct aw9106 *awi = gpiochip_get_data(irq_data_get_irq_chip_data(d));
	unsigned int bank;
	int ret;

	for (bank = 0; bank < 2; bank++) {
		ret = regmap_update_bits(awi->regmap, AW9106_IRQ_MASK + bank,
					 aw9106_bank_mask(bank),
					 aw9106_bank_bits(awi->irq_mask, bank));
		if (ret)
			dev_err_ratelimited(awi->dev, "IRQ mask write failed: %d\n", ret);
	}
	mutex_unlock(&awi->lock);
}

static const struct irq_chip aw9106_irq_chip = {
	.name = "aw9106",
	.irq_mask = aw9106_irq_mask,
	.irq_unmask = aw9106_irq_unmask,
	.irq_set_type = aw9106_irq_set_type,
	.irq_bus_lock = aw9106_irq_bus_lock,
	.irq_bus_sync_unlock = aw9106_irq_bus_sync_unlock,
	.flags = IRQCHIP_IMMUTABLE,
	GPIOCHIP_IRQ_RESOURCE_HELPERS,
};

static irqreturn_t aw9106_irq_thread(int irq, void *data)
{
	struct aw9106 *awi = data;
	unsigned long changed;
	unsigned int offset, child_irq;
	u8 state;
	int ret;

	mutex_lock(&awi->lock);
	ret = aw9106_read_inputs(awi, &state);
	if (!ret) {
		changed = (state ^ awi->input_state) & ~awi->irq_mask;
		awi->input_state = state;
	}
	mutex_unlock(&awi->lock);
	if (ret) {
		dev_err_ratelimited(awi->dev, "IRQ input read failed: %d\n", ret);
		return IRQ_HANDLED;
	}

	/* Child handlers may read GPIOs or mask IRQs, so run them without lock. */
	for_each_set_bit(offset, &changed, AW9106_NGPIO) {
		child_irq = irq_find_mapping(awi->gpio.irq.domain, offset);
		if (child_irq)
			handle_nested_irq(child_irq);
	}

	return IRQ_HANDLED;
}

static bool aw9106_readable_reg(struct device *dev, unsigned int reg)
{
	return reg <= AW9106_IRQ_MASK + 1 || reg == AW9106_ID ||
	       reg == AW9106_GPIO_MODE || reg == AW9106_GPIO_MODE + 1;
}

static bool aw9106_writeable_reg(struct device *dev, unsigned int reg)
{
	return (reg >= AW9106_OUTPUT && reg <= AW9106_IRQ_MASK + 1) ||
	       reg == AW9106_GPIO_MODE || reg == AW9106_GPIO_MODE + 1;
}

static bool aw9106_input_reg(struct device *dev, unsigned int reg)
{
	return reg == AW9106_INPUT || reg == AW9106_INPUT + 1;
}

static const struct regmap_config aw9106_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = AW9106_GPIO_MODE + 1,
	.readable_reg = aw9106_readable_reg,
	.writeable_reg = aw9106_writeable_reg,
	.volatile_reg = aw9106_input_reg,
	.precious_reg = aw9106_input_reg,
	.cache_type = REGCACHE_NONE,
};

static void aw9106_disable_irqs(void *data)
{
	struct aw9106 *awi = data;
	unsigned int bank;
	u8 state;

	mutex_lock(&awi->lock);
	awi->irq_mask = AW9106_GPIO_MASK;
	for (bank = 0; bank < 2; bank++)
		regmap_update_bits(awi->regmap, AW9106_IRQ_MASK + bank,
				   aw9106_bank_mask(bank), aw9106_bank_mask(bank));
	aw9106_read_inputs(awi, &state);
	mutex_unlock(&awi->lock);
}

static int aw9106_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct gpio_irq_chip *girq;
	struct gpio_desc *reset;
	struct aw9106 *awi;
	unsigned int id, bank;
	int ret;

	awi = devm_kzalloc(dev, sizeof(*awi), GFP_KERNEL);
	if (!awi)
		return -ENOMEM;
	awi->dev = dev;
	awi->irq_mask = AW9106_GPIO_MASK;
	ret = devm_mutex_init(dev, &awi->lock);
	if (ret)
		return ret;

	awi->regmap = devm_regmap_init_i2c(client, &aw9106_regmap);
	if (IS_ERR(awi->regmap))
		return dev_err_probe(dev, PTR_ERR(awi->regmap), "Cannot create regmap\n");

	/* reset-gpios is active low: assert, then release SHDN and let it settle. */
	reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(reset))
		return dev_err_probe(dev, PTR_ERR(reset), "Cannot request reset GPIO\n");
	usleep_range(50, 100);
	gpiod_set_value_cansleep(reset, 0);
	fsleep(5000);

	ret = regmap_read(awi->regmap, AW9106_ID, &id);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot read chip ID\n");
	if (id != AW9106_CHIP_ID)
		return dev_err_probe(dev, -ENODEV, "Unexpected chip ID %#x\n", id);

	/* Keep unrequested channels at reset defaults, including LED channels. */
	for (bank = 0; bank < 2; bank++) {
		ret = regmap_update_bits(awi->regmap, AW9106_IRQ_MASK + bank,
					 aw9106_bank_mask(bank), aw9106_bank_mask(bank));
		if (ret)
			return dev_err_probe(dev, ret, "Cannot mask interrupts\n");
	}
	ret = aw9106_read_inputs(awi, &awi->input_state);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot read initial inputs\n");

	awi->gpio.label = dev_name(dev);
	awi->gpio.parent = dev;
	awi->gpio.owner = THIS_MODULE;
	awi->gpio.base = -1;
	awi->gpio.ngpio = AW9106_NGPIO;
	awi->gpio.can_sleep = true;
	awi->gpio.request = aw9106_gpio_request;
	awi->gpio.get_direction = aw9106_gpio_get_direction;
	awi->gpio.direction_input = aw9106_gpio_direction_input;
	awi->gpio.direction_output = aw9106_gpio_direction_output;
	awi->gpio.get = aw9106_gpio_get;
	awi->gpio.set = aw9106_gpio_set;

	if (client->irq) {
		girq = &awi->gpio.irq;
		gpio_irq_chip_set_chip(girq, &aw9106_irq_chip);
		girq->default_type = IRQ_TYPE_NONE;
		girq->handler = handle_simple_irq;
		girq->threaded = true;
	}

	ret = devm_gpiochip_add_data(dev, &awi->gpio, awi);
	if (ret)
		return dev_err_probe(dev, ret, "Cannot register GPIO chip\n");

	if (client->irq) {
		ret = devm_request_threaded_irq(dev, client->irq, NULL,
						aw9106_irq_thread, IRQF_ONESHOT,
						dev_name(dev), awi);
		if (ret)
			return dev_err_probe(dev, ret, "Cannot request interrupt\n");
	}

	return devm_add_action_or_reset(dev, aw9106_disable_irqs, awi);
}

static const struct of_device_id aw9106_of_match[] = {
	{ .compatible = "awinic,aw9106" },
	{ }
};
MODULE_DEVICE_TABLE(of, aw9106_of_match);

static const struct i2c_device_id aw9106_id[] = {
	{ "aw9106" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, aw9106_id);

static struct i2c_driver aw9106_driver = {
	.driver = {
		.name = "aw9106-gpio",
		.of_match_table = aw9106_of_match,
	},
	.probe = aw9106_probe,
	.id_table = aw9106_id,
};
module_i2c_driver(aw9106_driver);

MODULE_DESCRIPTION("Awinic AW9106 GPIO controller");
MODULE_LICENSE("GPL");
