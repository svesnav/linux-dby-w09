// SPDX-License-Identifier: GPL-2.0-only
/* DBY-W09: read the already-running PM8150B GEN4 gauge without configuring it.
 * Register layout, shadow checks and conversions follow Huawei's GPL 4.19
 * fg-reg.h, fg-util.c and smb5-reg.h. The only optional write services the
 * firmware-enabled charger watchdog, as the stock driver's bark handler does.
 * Charge limits, protections, profiles and gauge SRAM are never changed.
 */
#include <linux/math64.h>
#include <linux/devm-helpers.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/unaligned.h>

#define MATEPAD_POWER_PATH_STATUS	0x110b
#define MATEPAD_USE_INPUT		(BIT(4) | BIT(3))
#define MATEPAD_VALID_INPUT		BIT(0)
#define MATEPAD_WDOG_CFG		0x1651
#define MATEPAD_WDOG_ON_PLUGIN		BIT(1)
#define MATEPAD_WDOG_PET		0x1643
#define MATEPAD_WDOG_PET_INTERVAL	(8 * HZ)

struct matepad_fg {
	struct device *dev;
	struct regmap *map;
	struct power_supply *battery, *ac;
	struct delayed_work poll;
	struct mutex lock;
	unsigned int base;
	bool valid, present, online;
	int capacity, voltage, current_ua, status;
	bool service_watchdog;
	unsigned long next_watchdog;
};

/* Keep the inherited watchdog armed; never enable charging or clear faults. */
static void matepad_fg_service_watchdog(struct matepad_fg *fg)
{
	unsigned int cfg;
	int ret;

	if (!fg->service_watchdog || !fg->valid || !fg->present || !fg->online)
		return;
	if (time_before(jiffies, fg->next_watchdog))
		return;
	ret = regmap_read(fg->map, MATEPAD_WDOG_CFG, &cfg);
	if (!ret && (cfg & MATEPAD_WDOG_ON_PLUGIN))
		ret = regmap_write(fg->map, MATEPAD_WDOG_PET, BIT(0));
	if (ret) {
		dev_warn_ratelimited(fg->dev, "Charger watchdog service failed: %d\n", ret);
		return;
	}
	fg->next_watchdog = jiffies + MATEPAD_WDOG_PET_INTERVAL;
}

static int matepad_fg_read_shadow(struct matepad_fg *fg, unsigned int reg,
				 unsigned int copy, u16 *value)
{
	u8 data[2], shadow[2];
	int i, ret;

	for (i = 0; i < 5; i++) {
		ret = regmap_bulk_read(fg->map, reg, data, sizeof(data));
		if (ret)
			return ret;
		ret = regmap_bulk_read(fg->map, copy, shadow, sizeof(shadow));
		if (ret)
			return ret;
		if (!memcmp(data, shadow, sizeof(data))) {
			*value = get_unaligned_le16(data);
			return 0;
		}
	}
	return -EAGAIN;
}

/* Called with lock held. Publish a complete sample only after all reads pass. */
static int matepad_fg_refresh(struct matepad_fg *fg)
{
	unsigned int enabled, ready, missing, path, chg;
	u8 soc[2];
	u16 voltage, raw_current;
	int capacity, status, ibat, ret, i;
	bool online;

	ret = regmap_read(fg->map, fg->base + 0x46, &enabled);
	if (ret)
		return ret;
	ret = regmap_read(fg->map, fg->base + 0x10, &ready);
	if (ret)
		return ret;
	if (!(enabled & BIT(7)) || !(ready & BIT(1)))
		return -ENODATA;
	for (i = 0; i < 5; i++) {
		ret = regmap_bulk_read(fg->map, fg->base + 0x09, soc, sizeof(soc));
		if (ret)
			return ret;
		if (soc[0] == soc[1])
			break;
	}
	if (i == 5)
		return -EAGAIN;
	ret = matepad_fg_read_shadow(fg, fg->base + 0x1a0, fg->base + 0x1a6, &voltage);
	if (ret)
		return ret;
	ret = matepad_fg_read_shadow(fg, fg->base + 0x1a2, fg->base + 0x1a8, &raw_current);
	if (ret)
		return ret;
	ret = regmap_read(fg->map, fg->base + 0x107, &missing);
	if (ret)
		return ret;
	ret = regmap_read(fg->map, MATEPAD_POWER_PATH_STATUS, &path);
	if (ret)
		return ret;
	ret = regmap_read(fg->map, 0x1006, &chg);
	if (ret)
		return ret;

	capacity = soc[0] == 255 ? 100 : soc[0] == 0 ? 0 :
		DIV_ROUND_CLOSEST((soc[0] - 1) * 98, 253) + 1;
	/* Hardware uses negative IBAT for charging. Keep the stock gauge sign. */
	ibat = div_s64((s64)(s16)raw_current * 488281, 1000);
	online = (path & MATEPAD_USE_INPUT) && (path & MATEPAD_VALID_INPUT);
	if (!online || ibat > 10000)
		status = POWER_SUPPLY_STATUS_DISCHARGING;
	else if (ibat < -10000)
		status = POWER_SUPPLY_STATUS_CHARGING;
	else if ((chg & 7) == 5 || capacity == 100)
		status = POWER_SUPPLY_STATUS_FULL;
	else
		status = POWER_SUPPLY_STATUS_NOT_CHARGING;

	fg->present = !(missing & BIT(2));
	fg->online = online;
	fg->capacity = capacity;
	fg->voltage = div_u64((u64)voltage * 122070, 1000);
	fg->current_ua = ibat;
	fg->status = status;
	return 0;
}

static int matepad_fg_get(struct power_supply *psy,
			 enum power_supply_property prop,
			 union power_supply_propval *val)
{
	struct matepad_fg *fg = power_supply_get_drvdata(psy);
	int ret = 0;

	mutex_lock(&fg->lock);
	if (!fg->valid) {
		ret = -ENODATA;
		goto out;
	}
	switch (prop) {
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = fg->present;
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = fg->online;
		break;
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = fg->status;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = fg->capacity;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = fg->voltage;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = fg->current_ua;
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_SYSTEM;
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = "MatePad 11 battery";
		break;
	default:
		ret = -EINVAL;
	}
out:
	mutex_unlock(&fg->lock);
	return ret;
}

static void matepad_fg_poll(struct work_struct *work)
{
	struct matepad_fg *fg = container_of(to_delayed_work(work), struct matepad_fg, poll);
	bool changed, valid, online, present;
	int capacity, status;

	mutex_lock(&fg->lock);
	valid = fg->valid;
	online = fg->online;
	present = fg->present;
	capacity = fg->capacity;
	status = fg->status;
	fg->valid = !matepad_fg_refresh(fg);
	matepad_fg_service_watchdog(fg);
	changed = valid != fg->valid || online != fg->online || present != fg->present ||
		capacity != fg->capacity || status != fg->status;
	mutex_unlock(&fg->lock);
	if (changed) {
		power_supply_changed(fg->battery);
		power_supply_changed(fg->ac);
	}
	schedule_delayed_work(&fg->poll, HZ);
}

static const enum power_supply_property battery_props[] = {
	POWER_SUPPLY_PROP_PRESENT, POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CAPACITY, POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW, POWER_SUPPLY_PROP_SCOPE,
	POWER_SUPPLY_PROP_MODEL_NAME,
};
static const enum power_supply_property ac_props[] = { POWER_SUPPLY_PROP_ONLINE };
static const struct power_supply_desc battery_desc = {
	.name = "battery", .type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = battery_props, .num_properties = ARRAY_SIZE(battery_props),
	.get_property = matepad_fg_get,
};
static const struct power_supply_desc ac_desc = {
	.name = "matepad-charger", .type = POWER_SUPPLY_TYPE_MAINS,
	.properties = ac_props, .num_properties = ARRAY_SIZE(ac_props),
	.get_property = matepad_fg_get,
};

static int matepad_fg_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct power_supply_config config = {};
	struct matepad_fg *fg;
	u8 type[2];
	int ret;

	fg = devm_kzalloc(dev, sizeof(*fg), GFP_KERNEL);
	if (!fg)
		return -ENOMEM;
	fg->dev = dev;
	fg->service_watchdog = device_property_read_bool(dev, "huawei,service-charger-watchdog");
	fg->next_watchdog = jiffies;
	fg->map = dev_get_regmap(dev->parent, NULL);
	if (!fg->map)
		return -ENODEV;
	ret = device_property_read_u32(dev, "reg", &fg->base);
	if (ret)
		return ret;
	ret = regmap_bulk_read(fg->map, fg->base + 4, type, sizeof(type));
	if (ret)
		return ret;
	if (type[0] != 0x0d || type[1] != 0x10)
		return dev_err_probe(dev, -ENODEV, "Unexpected fuel gauge type\n");
	mutex_init(&fg->lock);
	fg->valid = !matepad_fg_refresh(fg);
	matepad_fg_service_watchdog(fg);
	config.drv_data = fg;
	config.fwnode = dev_fwnode(dev);
	fg->battery = devm_power_supply_register(dev, &battery_desc, &config);
	if (IS_ERR(fg->battery))
		return PTR_ERR(fg->battery);
	fg->ac = devm_power_supply_register(dev, &ac_desc, &config);
	if (IS_ERR(fg->ac))
		return PTR_ERR(fg->ac);
	ret = devm_delayed_work_autocancel(dev, &fg->poll, matepad_fg_poll);
	if (ret)
		return ret;
	schedule_delayed_work(&fg->poll, HZ);
	dev_info(dev, "GEN4 battery reporting enabled; charger watchdog service %s\n",
		 fg->service_watchdog ? "enabled" : "disabled");
	return 0;
}

static const struct of_device_id matepad_fg_of_match[] = {
	{ .compatible = "huawei,dby-w09-fg" }, {}
};
MODULE_DEVICE_TABLE(of, matepad_fg_of_match);
static struct platform_driver matepad_fg_driver = {
	.probe = matepad_fg_probe,
	.driver = { .name = "matepad-fg", .of_match_table = matepad_fg_of_match },
};
module_platform_driver(matepad_fg_driver);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Read-only Huawei MatePad 11 PM8150B battery reporting");
