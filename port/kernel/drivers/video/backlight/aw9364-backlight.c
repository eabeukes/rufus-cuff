// SPDX-License-Identifier: GPL-2.0-only
/*
 * Awinic AW9364 LED driver as an LCD backlight (Rufus Cuff / Wiite C10 watch).
 *
 * One enable line: EN high = brightest step; every low pulse (0.5..500 us) on EN steps
 * one level dimmer, wrapping from the dimmest back to the brightest; EN low for more than
 * 2.5 ms shuts the chip down. 16 steps. Brightness 0 = off, 1..16 = dimmest..brightest.
 *
 * DT: compatible = "awinic,aw9364"; enable-gpios = <...>; default-brightness = <N>;
 */
#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/spinlock.h>

#define AW9364_STEPS		16
#define AW9364_PULSE_US		2	/* low and high time of a dimming pulse */
#define AW9364_SHUTDOWN_US	3000	/* > 2.5 ms */
#define AW9364_READY_US		50

struct aw9364 {
	struct gpio_desc *en;
	int level;			/* 0 = off, 1..16 */
	spinlock_t lock;
};

/* From the brightest step (16), go down to level with (16 - level) pulses. */
static void aw9364_pulses(struct aw9364 *aw, int n)
{
	unsigned long flags;

	spin_lock_irqsave(&aw->lock, flags);
	while (n--) {
		gpiod_set_value(aw->en, 0);
		udelay(AW9364_PULSE_US);
		gpiod_set_value(aw->en, 1);
		udelay(AW9364_PULSE_US);
	}
	spin_unlock_irqrestore(&aw->lock, flags);
}

static int aw9364_update_status(struct backlight_device *bl)
{
	struct aw9364 *aw = bl_get_data(bl);
	int level = backlight_get_brightness(bl);

	if (level == aw->level)
		return 0;

	if (level == 0) {
		gpiod_set_value_cansleep(aw->en, 0);
		usleep_range(AW9364_SHUTDOWN_US, AW9364_SHUTDOWN_US + 500);
		aw->level = 0;
		return 0;
	}

	if (aw->level == 0) {
		gpiod_set_value_cansleep(aw->en, 1);
		udelay(AW9364_READY_US);
		aw->level = AW9364_STEPS;
	}

	/* Steps only go down (dimmer) and wrap from 1 back to 16. */
	aw9364_pulses(aw, (aw->level - level + AW9364_STEPS) % AW9364_STEPS);
	aw->level = level;
	return 0;
}

static const struct backlight_ops aw9364_ops = {
	.options = BL_CORE_SUSPENDRESUME,
	.update_status = aw9364_update_status,
};

static int aw9364_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.max_brightness = AW9364_STEPS,
		.brightness = AW9364_STEPS,
		.scale = BACKLIGHT_SCALE_NON_LINEAR,
	};
	struct backlight_device *bl;
	struct aw9364 *aw;
	u32 def;

	aw = devm_kzalloc(dev, sizeof(*aw), GFP_KERNEL);
	if (!aw)
		return -ENOMEM;
	spin_lock_init(&aw->lock);

	/* The firmware left the chip on at an unknown step: shut it down to know the state. */
	aw->en = devm_gpiod_get(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(aw->en))
		return dev_err_probe(dev, PTR_ERR(aw->en), "no enable gpio\n");
	usleep_range(AW9364_SHUTDOWN_US, AW9364_SHUTDOWN_US + 500);
	aw->level = 0;

	if (!device_property_read_u32(dev, "default-brightness", &def))
		props.brightness = clamp_t(u32, def, 0, AW9364_STEPS);

	bl = devm_backlight_device_register(dev, dev_name(dev), dev, aw, &aw9364_ops, &props);
	if (IS_ERR(bl))
		return dev_err_probe(dev, PTR_ERR(bl), "backlight register failed\n");

	return backlight_update_status(bl);
}

static const struct of_device_id aw9364_of_match[] = {
	{ .compatible = "awinic,aw9364" },
	{ }
};
MODULE_DEVICE_TABLE(of, aw9364_of_match);

static struct platform_driver aw9364_driver = {
	.probe = aw9364_probe,
	.driver = {
		.name = "aw9364-backlight",
		.of_match_table = aw9364_of_match,
	},
};
module_platform_driver(aw9364_driver);

MODULE_DESCRIPTION("Awinic AW9364 pulse-dimmed backlight");
MODULE_LICENSE("GPL");
