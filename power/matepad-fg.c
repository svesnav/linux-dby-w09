// SPDX-License-Identifier: GPL-2.0-only
/* DBY-W09: read the already-running PM8150B GEN4 gauge without configuring it.
 * Register layout, shadow checks and conversions follow Huawei's GPL 4.19
 * fg-reg.h, fg-util.c and smb5-reg.h. Two optional writes: servicing the
 * firmware-enabled charger watchdog, as the stock driver's bark handler does,
 * and the USB input current limit from the USB-C supply, as the stock
 * smblib_set_icl_current() does. Charge (battery) limits, protections,
 * profiles and gauge SRAM are never changed.
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
/* SMB5 input current limit (smb5-reg.h), as smblib_set_icl_current() sets it */
#define MATEPAD_CMD_ICL_OVERRIDE	0x1342
#define MATEPAD_ICL_OVERRIDE		BIT(0)
#define MATEPAD_USBIN_LOAD_CFG		0x1365
#define MATEPAD_ICL_OVERRIDE_AFTER_APSD	BIT(4)
#define MATEPAD_USBIN_ICL_OPTIONS	0x1366
#define MATEPAD_USBIN_MODE_CHG		BIT(0)
#define MATEPAD_USBIN_CURRENT_LIMIT	0x1370
#define MATEPAD_ICL_STEP_UA		50000
#define MATEPAD_ICL_MIN_UA		500000
#define MATEPAD_ICL_MAX_UA		3000000

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
	bool set_icl;
	int icl_ua;			/* limit set from the USB-C supply; 0: firmware's */
	unsigned int fw_icl[4];		/* firmware's override, load, options, limit */
};

static const unsigned int matepad_icl_regs[4] = {
	MATEPAD_CMD_ICL_OVERRIDE, MATEPAD_USBIN_LOAD_CFG,
	MATEPAD_USBIN_ICL_OPTIONS, MATEPAD_USBIN_CURRENT_LIMIT,
};

/*
 * Input current limit from the USB-C supply (TCPM: Type-C current or the PD
 * contract). Started with power already connected, the charger keeps the
 * 500 mA USB limit that its detection chose at boot and the battery does not
 * charge with the screen on; a replug used to be the only way out. With a
 * current from the USB-C side, set it in high-current mode as the stock
 * smblib_set_icl_current() does; without one (unplugged, default USB power),
 * give the charger back the firmware's settings and its own detection.
 */
static void matepad_fg_update_icl(struct matepad_fg *fg, bool online)
{
	union power_supply_propval val = {};
	int icl_ua = 0, ret, i;

	if (online && !power_supply_get_property_from_supplier(fg->ac,
			POWER_SUPPLY_PROP_CURRENT_MAX, &val) && val.intval > 0)
		icl_ua = rounddown(clamp(val.intval, MATEPAD_ICL_MIN_UA, MATEPAD_ICL_MAX_UA),
				   MATEPAD_ICL_STEP_UA);
	if (icl_ua == fg->icl_ua)
		return;

	if (icl_ua) {
		ret = regmap_write(fg->map, MATEPAD_USBIN_CURRENT_LIMIT,
				   icl_ua / MATEPAD_ICL_STEP_UA);
		if (!ret)
			ret = regmap_update_bits(fg->map, MATEPAD_USBIN_ICL_OPTIONS,
						 MATEPAD_USBIN_MODE_CHG, MATEPAD_USBIN_MODE_CHG);
		if (!ret)
			ret = regmap_update_bits(fg->map, MATEPAD_CMD_ICL_OVERRIDE,
						 MATEPAD_ICL_OVERRIDE, 0);
		if (!ret)
			ret = regmap_update_bits(fg->map, MATEPAD_USBIN_LOAD_CFG,
						 MATEPAD_ICL_OVERRIDE_AFTER_APSD,
						 MATEPAD_ICL_OVERRIDE_AFTER_APSD);
	} else {
		for (i = 0, ret = 0; i < ARRAY_SIZE(matepad_icl_regs) && !ret; i++)
			ret = regmap_write(fg->map, matepad_icl_regs[i], fg->fw_icl[i]);
	}
	if (ret) {
		dev_warn_ratelimited(fg->dev, "Input current limit: %d\n", ret);
		return;
	}
	if (icl_ua)
		dev_info(fg->dev, "input current limit %d mA from USB-C\n", icl_ua / 1000);
	else
		dev_info(fg->dev, "input current limit back to the charger's detection\n");
	fg->icl_ua = icl_ua;
}

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
	if (fg->set_icl && fg->valid)
		matepad_fg_update_icl(fg, fg->online);
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
	int ret, i;

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
	fg->set_icl = device_property_read_bool(dev, "huawei,set-input-current-limit");
	for (i = 0; fg->set_icl && i < ARRAY_SIZE(matepad_icl_regs); i++) {
		ret = regmap_read(fg->map, matepad_icl_regs[i], &fg->fw_icl[i]);
		if (ret)
			return ret;
	}
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
	dev_info(dev, "GEN4 battery reporting enabled; charger watchdog service %s; input current limit from USB-C %s\n",
		 fg->service_watchdog ? "enabled" : "disabled", fg->set_icl ? "enabled" : "disabled");
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
MODULE_DESCRIPTION("Huawei MatePad 11 PM8150B battery reporting and USB-C input current limit");
