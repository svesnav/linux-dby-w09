// SPDX-License-Identifier: GPL-2.0-only
/*
 * Richtek RT8555 LED backlight driver, as used on the Huawei MatePad 11
 * (DBY-W09).  Register values follow the stock HarmonyOS hw_rt8555 node:
 * after the enable GPIO goes high, eleven init writes, then a 10-bit level
 * in 0x04 (bits 7:0) and 0x05 (bits 9:8).
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>

#define RT8555_REG_LEVEL_LSB	0x04
#define RT8555_REG_LEVEL_MSB	0x05
#define RT8555_MAX_LEVEL	1023

struct rt8555 {
	struct regmap *regmap;
	struct gpio_desc *enable;
	bool powered;
};

/* hw_rt8555,backlight_ic_kernel_init_cmd */
static const struct reg_sequence rt8555_init[] = {
	{ 0x00, 0xc3 }, { 0x01, 0xd3 }, { 0x02, 0xb7 }, { 0x03, 0x00 },
	{ 0x08, 0x10 }, { 0x09, 0x7f }, { 0x0a, 0x04 }, { 0x0b, 0x00 },
	{ 0x0d, 0x00 }, { 0x0e, 0x00 }, { 0x50, 0x01 },
};

static int rt8555_power(struct rt8555 *rt, bool on)
{
	int ret;

	if (rt->powered == on)
		return 0;
	if (!on) {
		gpiod_set_value_cansleep(rt->enable, 0);
		rt->powered = false;
		return 0;
	}
	gpiod_set_value_cansleep(rt->enable, 1);
	msleep(30);	/* hw_rt8555,bl_on_mdelay */
	ret = regmap_multi_reg_write(rt->regmap, rt8555_init, ARRAY_SIZE(rt8555_init));
	if (ret) {
		gpiod_set_value_cansleep(rt->enable, 0);
		return ret;
	}
	rt->powered = true;
	return 0;
}

static int rt8555_update_status(struct backlight_device *bl)
{
	struct rt8555 *rt = bl_get_data(bl);
	unsigned int level = backlight_get_brightness(bl);
	int ret;

	if (!level)
		return rt8555_power(rt, false);

	ret = rt8555_power(rt, true);
	if (ret)
		return ret;
	ret = regmap_write(rt->regmap, RT8555_REG_LEVEL_LSB, level & 0xff);
	if (ret)
		return ret;
	return regmap_update_bits(rt->regmap, RT8555_REG_LEVEL_MSB, 0x03, level >> 8);
}

static const struct backlight_ops rt8555_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = rt8555_update_status,
};

static const struct regmap_config rt8555_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x50,
};

static int rt8555_probe(struct i2c_client *client)
{
	struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.max_brightness = RT8555_MAX_LEVEL,
		.brightness = 512,
		.scale = BACKLIGHT_SCALE_LINEAR,
	};
	struct device *dev = &client->dev;
	struct backlight_device *bl;
	struct rt8555 *rt;

	rt = devm_kzalloc(dev, sizeof(*rt), GFP_KERNEL);
	if (!rt)
		return -ENOMEM;
	rt->regmap = devm_regmap_init_i2c(client, &rt8555_regmap);
	if (IS_ERR(rt->regmap))
		return PTR_ERR(rt->regmap);
	rt->enable = devm_gpiod_get(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(rt->enable))
		return dev_err_probe(dev, PTR_ERR(rt->enable), "enable gpio\n");

	/* Stay dark until the panel driver enables the backlight. */
	props.power = BACKLIGHT_POWER_OFF;
	bl = devm_backlight_device_register(dev, "panel0-backlight", dev, rt,
					    &rt8555_ops, &props);
	if (IS_ERR(bl))
		return dev_err_probe(dev, PTR_ERR(bl), "register backlight\n");
	i2c_set_clientdata(client, bl);
	return 0;
}

static const struct of_device_id rt8555_of_match[] = {
	{ .compatible = "richtek,rt8555" },
	{ }
};
MODULE_DEVICE_TABLE(of, rt8555_of_match);

static struct i2c_driver rt8555_driver = {
	.driver = {
		.name = "rt8555-backlight",
		.of_match_table = rt8555_of_match,
	},
	.probe = rt8555_probe,
};
module_i2c_driver(rt8555_driver);

MODULE_DESCRIPTION("Richtek RT8555 backlight driver");
MODULE_LICENSE("GPL");
