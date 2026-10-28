// SPDX-License-Identifier: GPL-2.0
/*
 * Bosch BMA423 accelerometer (Rufus Cuff, I2C1 0x18): raw acceleration and temperature.
 *
 * Plain accelerometer data only: 12 bit, +-4 g, 100 Hz. The step counter, tilt and wake
 * gestures run in the chip's feature engine, which needs Bosch's config blob uploaded
 * first; that is not done here, and the data registers work without it.
 */
#include <linux/bits.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/iio/iio.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/regmap.h>

#define BMA423_REG_CHIP_ID	0x00
#define BMA423_CHIP_ID		0x13
#define BMA423_REG_ACC_X_LSB	0x12	/* x, y, z: LSB then MSB; reading the LSB latches the MSB */
#define BMA423_REG_TEMP		0x22	/* s8, 0 = 23 C; 0x80 = invalid */
#define BMA423_REG_ACC_CONF	0x40
#define BMA423_REG_ACC_RANGE	0x41
#define BMA423_REG_PWR_CONF	0x7c
#define BMA423_REG_PWR_CTRL	0x7d

#define BMA423_ACC_CONF_100HZ	0xa8	/* ODR 100 Hz, normal filter, performance mode */
#define BMA423_RANGE_4G		0x01
#define BMA423_PWR_CTRL_ACC_EN	BIT(2)

/* +-4 g over 12 bits: 512 LSB/g -> 9.80665 / 512 m/s^2 per LSB. */
#define BMA423_SCALE_MICRO	19153

struct bma423 {
	struct regmap *regmap;
};

#define BMA423_ACC_CHANNEL(_axis, _index) {				\
	.type = IIO_ACCEL,						\
	.modified = 1,							\
	.channel2 = IIO_MOD_##_axis,					\
	.address = _index,						\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),			\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE) |		\
				    BIT(IIO_CHAN_INFO_SAMP_FREQ),	\
}

static const struct iio_chan_spec bma423_channels[] = {
	BMA423_ACC_CHANNEL(X, 0),
	BMA423_ACC_CHANNEL(Y, 1),
	BMA423_ACC_CHANNEL(Z, 2),
	{
		.type = IIO_TEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_PROCESSED),
	},
};

static int bma423_read_raw(struct iio_dev *indio_dev, struct iio_chan_spec const *chan,
			   int *val, int *val2, long mask)
{
	struct bma423 *bma = iio_priv(indio_dev);
	unsigned int temp;
	__le16 raw;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = regmap_bulk_read(bma->regmap, BMA423_REG_ACC_X_LSB + 2 * chan->address,
				       &raw, sizeof(raw));
		if (ret)
			return ret;
		*val = sign_extend32(le16_to_cpu(raw) >> 4, 11);
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_PROCESSED:
		ret = regmap_read(bma->regmap, BMA423_REG_TEMP, &temp);
		if (ret)
			return ret;
		if (temp == 0x80)
			return -ENODATA;
		*val = ((s8)temp + 23) * 1000;	/* millidegrees */
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		*val = 0;
		*val2 = BMA423_SCALE_MICRO;
		return IIO_VAL_INT_PLUS_MICRO;
	case IIO_CHAN_INFO_SAMP_FREQ:
		*val = 100;
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static const struct iio_info bma423_info = {
	.read_raw = bma423_read_raw,
};

static const struct regmap_config bma423_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x7e,
};

static void bma423_disable(void *data)
{
	struct bma423 *bma = data;

	regmap_clear_bits(bma->regmap, BMA423_REG_PWR_CTRL, BMA423_PWR_CTRL_ACC_EN);
}

static int bma423_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct iio_dev *indio_dev;
	struct bma423 *bma;
	unsigned int id;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*bma));
	if (!indio_dev)
		return -ENOMEM;
	bma = iio_priv(indio_dev);

	bma->regmap = devm_regmap_init_i2c(client, &bma423_regmap_config);
	if (IS_ERR(bma->regmap))
		return PTR_ERR(bma->regmap);

	ret = regmap_read(bma->regmap, BMA423_REG_CHIP_ID, &id);
	if (ret)
		return dev_err_probe(dev, ret, "cannot read the chip ID\n");
	if (id != BMA423_CHIP_ID)
		return dev_err_probe(dev, -ENODEV, "chip ID 0x%02x, not a BMA423\n", id);

	/* Advanced power save off (it limits register access), then 100 Hz, +-4 g, on. */
	ret = regmap_write(bma->regmap, BMA423_REG_PWR_CONF, 0);
	if (ret)
		return ret;
	usleep_range(450, 1000);
	ret = regmap_write(bma->regmap, BMA423_REG_ACC_CONF, BMA423_ACC_CONF_100HZ);
	if (!ret)
		ret = regmap_write(bma->regmap, BMA423_REG_ACC_RANGE, BMA423_RANGE_4G);
	if (!ret)
		ret = regmap_set_bits(bma->regmap, BMA423_REG_PWR_CTRL, BMA423_PWR_CTRL_ACC_EN);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, bma423_disable, bma);
	if (ret)
		return ret;

	indio_dev->name = "bma423";
	indio_dev->info = &bma423_info;
	indio_dev->channels = bma423_channels;
	indio_dev->num_channels = ARRAY_SIZE(bma423_channels);
	indio_dev->modes = INDIO_DIRECT_MODE;

	return devm_iio_device_register(dev, indio_dev);
}

static const struct of_device_id bma423_of_match[] = {
	{ .compatible = "bosch,bma423" },
	{ }
};
MODULE_DEVICE_TABLE(of, bma423_of_match);

static const struct i2c_device_id bma423_id[] = {
	{ "bma423" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, bma423_id);

static struct i2c_driver bma423_driver = {
	.driver = {
		.name = "bma423",
		.of_match_table = bma423_of_match,
	},
	.probe = bma423_probe,
	.id_table = bma423_id,
};
module_i2c_driver(bma423_driver);

MODULE_DESCRIPTION("Bosch BMA423 accelerometer (raw data)");
MODULE_LICENSE("GPL");
