// SPDX-License-Identifier: GPL-2.0
/*
 * Sony IMX882 probe-only driver
 *
 * The OnePlus Nord 4's main camera is a Sony IMX882 on CCI0 master 0 at 0x1a
 * (CSIPHY0), with MCLK0 at 24 MHz on TLMM 64 and reset on TLMM 15. The rails
 * are the AW37004's AVDD2/DVDD2 plus L23B for I/O.
 *
 * Nothing in the public or extracted sources carries this module's register
 * programme, so this driver is deliberately probe-only: it powers the part,
 * reads the 16-bit chip ID at 0x0016/0x0017 and the diagnostic pair at
 * 0x0000/0x0001, logs every value and never fails the module load on an
 * unexpected answer. Streaming waits for a mode table and a decided PHY
 * (D-PHY vs C-PHY).
 *
 * Modelled on drivers/media/i2c/s5k3p9.c (16-bit register address, 8-bit
 * data).
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#include <media/media-entity.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>

#define IMX882_MCLK_FREQ		(24000000UL)

#define IMX882_REG_CHIP_ID_MSB		0x0016
#define IMX882_REG_CHIP_ID_LSB		0x0017
#define IMX882_REG_DIAG_MSB		0x0000
#define IMX882_REG_DIAG_LSB		0x0001

struct imx882_supply {
	const char *name;
	unsigned int ua;
};

static const struct imx882_supply imx882_supplies[] = {
	{ "avdd", 60000 },
	{ "dvdd", 200000 },
	{ "dovdd", 1000 },
};

static const struct regmap_config imx882_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = 0xffff,
};

struct imx882 {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct regmap *regmap;
	struct clk *mclk;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[ARRAY_SIZE(imx882_supplies)];
};

static int imx882_power_on(struct imx882 *sensor)
{
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(imx882_supplies),
				    sensor->supplies);
	if (ret)
		return ret;

	ret = clk_prepare_enable(sensor->mclk);
	if (ret)
		goto err_regulators;

	/* reset is active low: logical 0 releases the sensor */
	gpiod_set_value_cansleep(sensor->reset_gpio, 0);
	usleep_range(1000, 2000);

	return 0;

err_regulators:
	regulator_bulk_disable(ARRAY_SIZE(imx882_supplies), sensor->supplies);
	return ret;
}

static void imx882_power_off(struct imx882 *sensor)
{
	gpiod_set_value_cansleep(sensor->reset_gpio, 1);
	clk_disable_unprepare(sensor->mclk);
	regulator_bulk_disable(ARRAY_SIZE(imx882_supplies), sensor->supplies);
}

static const struct v4l2_subdev_ops imx882_subdev_ops = {
};

static void imx882_log_ids(struct imx882 *sensor, struct i2c_client *client)
{
	struct device *dev = &client->dev;
	u32 msb = 0, lsb = 0, diag_msb = 0, diag_lsb = 0;
	int ret;

	ret = regmap_read(sensor->regmap, IMX882_REG_CHIP_ID_MSB, &msb);
	if (!ret)
		ret = regmap_read(sensor->regmap, IMX882_REG_CHIP_ID_LSB, &lsb);
	if (ret)
		dev_info(dev, "chip ID read failed (%d); the power sequence is unverified\n",
			 ret);
	else
		dev_info(dev, "chip ID 0x%04x from 0x0016=0x%02x 0x0017=0x%02x at 0x%02x\n",
			 (msb << 8) | (lsb & 0xff), msb, lsb, client->addr);

	ret = regmap_read(sensor->regmap, IMX882_REG_DIAG_MSB, &diag_msb);
	if (!ret)
		ret = regmap_read(sensor->regmap, IMX882_REG_DIAG_LSB,
				  &diag_lsb);
	if (ret)
		dev_info(dev, "diagnostic read failed (%d)\n", ret);
	else
		dev_info(dev, "diagnostic 0x0000=0x%02x 0x0001=0x%02x\n",
			 diag_msb, diag_lsb);
}

static int imx882_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct imx882 *sensor;
	unsigned long freq;
	int ret, i;

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	v4l2_i2c_subdev_init(&sensor->sd, client, &imx882_subdev_ops);
	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	sensor->regmap = devm_regmap_init_i2c(client, &imx882_regmap_config);
	if (IS_ERR(sensor->regmap))
		return dev_err_probe(dev, PTR_ERR(sensor->regmap),
				     "failed to init regmap\n");

	sensor->mclk = devm_v4l2_sensor_clk_get(dev, NULL);
	if (IS_ERR(sensor->mclk))
		return dev_err_probe(dev, PTR_ERR(sensor->mclk),
				     "failed to get MCLK\n");

	freq = clk_get_rate(sensor->mclk);
	if (freq != IMX882_MCLK_FREQ)
		return dev_err_probe(dev, -EINVAL,
				     "MCLK %lu is not %lu\n", freq,
				     IMX882_MCLK_FREQ);

	sensor->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(sensor->reset_gpio),
				     "failed to get reset GPIO\n");

	for (i = 0; i < ARRAY_SIZE(imx882_supplies); i++) {
		sensor->supplies[i].supply = imx882_supplies[i].name;
		sensor->supplies[i].init_load_uA = imx882_supplies[i].ua;
	}
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(imx882_supplies),
				      sensor->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
	if (ret)
		return ret;

	/* power once, report what the part answers, power off */
	ret = imx882_power_on(sensor);
	if (ret)
		goto err_entity;

	imx882_log_ids(sensor, client);
	imx882_power_off(sensor);

	ret = v4l2_async_register_subdev_sensor(&sensor->sd);
	if (ret)
		goto err_entity;

	dev_info(dev, "imx882 registered (probe-only: no mode tables)\n");
	return 0;

err_entity:
	media_entity_cleanup(&sensor->sd.entity);
	return ret;
}

static void imx882_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
}

static const struct of_device_id imx882_of_match[] = {
	{ .compatible = "sony,imx882" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, imx882_of_match);

static struct i2c_driver imx882_i2c_driver = {
	.driver = {
		.name = "imx882",
		.of_match_table = imx882_of_match,
	},
	.probe = imx882_probe,
	.remove = imx882_remove,
};

module_i2c_driver(imx882_i2c_driver);

MODULE_DESCRIPTION("Sony IMX882 probe-only sensor driver");
MODULE_LICENSE("GPL");
