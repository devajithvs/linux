// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung S5K3P9 16 MP CMOS image sensor driver (skeleton)
 *
 * The front camera of the OnePlus Nord 4 (QTech module, AP I2C bus
 * qupv3_se8 at 0x20, CSIPHY4). The wiring below comes from the shipped
 * vendor device tree; the register programme does not exist in any public
 * source (F0380): the vendor kernel ships no s5k3p9.c, and the CamX module
 * blobs are Chromatix parameter files. The mode and init tables are therefore
 * empty, and streaming will not produce frames until they are filled from a
 * datasheet or a vendor driver. Everything else - power, clock, reset, the
 * subdev and the pads - is complete, so the probe and the media graph can be
 * tested today.
 *
 * Modelled on drivers/media/i2c/s5kjn1.c (same register model: 16-bit
 * address, 8-bit data).
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#include <media/media-entity.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define S5K3P9_MCLK_FREQ		(19200000UL)
#define S5K3P9_DATA_LANES		4
#define S5K3P9_LINK_FREQ_DEFAULT	(600000000ULL)	/* TODO: from the datasheet */
#define S5K3P9_WIDTH			4608
#define S5K3P9_HEIGHT			3456

#define S5K3P9_REG_CHIP_ID		0x0000
#define S5K3P9_REG_CTRL_MODE		0x0100
#define S5K3P9_MODE_STREAMING		BIT(0)
#define S5K3P9_REG_ORIENTATION		0x0101
#define S5K3P9_VFLIP			BIT(1)
#define S5K3P9_HFLIP			BIT(0)

struct s5k3p9_reg {
	u16 reg;
	u8 val;
};

struct s5k3p9_supply {
	const char *name;
	unsigned int ua;
};

static const struct s5k3p9_supply s5k3p9_supplies[] = {
	{ "vana", 60000 },
	{ "vdig", 200000 },
	{ "vio", 1000 },
};

/*
 * The init and mode tables. Empty on purpose: no public source carries them
 * (F0380). When they arrive, they go here and the mode below is filled in -
 * nothing else in this driver changes.
 */
static const struct s5k3p9_reg s5k3p9_init_regs[] = {
	/* { 0x0a02, 0x00 }, */
};

static const struct s5k3p9_reg s5k3p9_mode_regs[] = {
	/* 4608x3456 10-bit; TODO: the vendor sequence */
};

static const struct regmap_config s5k3p9_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = 0xffff,
};

struct s5k3p9_mode {
	u32 width;
	u32 height;
	const struct s5k3p9_reg *regs;
	unsigned int num_regs;
};

static const struct s5k3p9_mode s5k3p9_modes[] = {
	{
		.width = S5K3P9_WIDTH,
		.height = S5K3P9_HEIGHT,
		.regs = s5k3p9_mode_regs,
		.num_regs = ARRAY_SIZE(s5k3p9_mode_regs),
	},
};

static const u32 s5k3p9_mbus_codes[] = {
	MEDIA_BUS_FMT_SGRBG10_1X10,
};

struct s5k3p9 {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct regmap *regmap;
	struct clk *mclk;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[ARRAY_SIZE(s5k3p9_supplies)];
	struct v4l2_ctrl_handler ctrl_handler;

	const struct s5k3p9_mode *mode;
	struct v4l2_mbus_framefmt fmt;
	unsigned long link_freq_bitmap;
	bool streaming;
};

static inline struct s5k3p9 *to_s5k3p9(struct v4l2_subdev *sd)
{
	return container_of(sd, struct s5k3p9, sd);
}

static int s5k3p9_write_table(struct s5k3p9 *sensor,
			      const struct s5k3p9_reg *regs, unsigned int n)
{
	unsigned int i;
	int ret;

	for (i = 0; i < n; i++) {
		ret = regmap_write(sensor->regmap, regs[i].reg, regs[i].val);
		if (ret)
			return ret;
	}
	return 0;
}

static int s5k3p9_power_on(struct s5k3p9 *sensor)
{
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(s5k3p9_supplies),
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
	regulator_bulk_disable(ARRAY_SIZE(s5k3p9_supplies), sensor->supplies);
	return ret;
}

static void s5k3p9_power_off(struct s5k3p9 *sensor)
{
	gpiod_set_value_cansleep(sensor->reset_gpio, 1);
	clk_disable_unprepare(sensor->mclk);
	regulator_bulk_disable(ARRAY_SIZE(s5k3p9_supplies), sensor->supplies);
}

static int s5k3p9_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct s5k3p9 *sensor = to_s5k3p9(sd);
	int ret = 0;

	if (enable) {
		ret = pm_runtime_resume_and_get(sd->dev);
		if (ret)
			return ret;

		ret = s5k3p9_write_table(sensor, s5k3p9_init_regs,
					 ARRAY_SIZE(s5k3p9_init_regs));
		if (ret)
			goto err_pm;

		ret = s5k3p9_write_table(sensor, sensor->mode->regs,
					 sensor->mode->num_regs);
		if (ret)
			goto err_pm;

		ret = regmap_write(sensor->regmap, S5K3P9_REG_CTRL_MODE,
				   S5K3P9_MODE_STREAMING);
		if (ret)
			goto err_pm;

		sensor->streaming = true;
	} else {
		regmap_write(sensor->regmap, S5K3P9_REG_CTRL_MODE, 0);
		sensor->streaming = false;
		pm_runtime_put(sd->dev);
	}

	return 0;

err_pm:
	pm_runtime_put(sd->dev);
	return ret;
}

static int s5k3p9_get_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *format)
{
	struct s5k3p9 *sensor = to_s5k3p9(sd);

	format->format = sensor->fmt;
	return 0;
}

static int s5k3p9_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *format)
{
	struct s5k3p9 *sensor = to_s5k3p9(sd);
	struct v4l2_mbus_framefmt *fmt = &format->format;

	fmt->code = s5k3p9_mbus_codes[0];
	fmt->width = clamp(fmt->width, 1U, S5K3P9_WIDTH);
	fmt->height = clamp(fmt->height, 1U, S5K3P9_HEIGHT);
	fmt->field = V4L2_FIELD_NONE;

	/* one mode today: the active format is always that one */
	if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		sensor->fmt = *fmt;

	return 0;
}

static int s5k3p9_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index >= ARRAY_SIZE(s5k3p9_mbus_codes))
		return -EINVAL;

	code->code = s5k3p9_mbus_codes[code->index];
	return 0;
}

static int s5k3p9_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index >= ARRAY_SIZE(s5k3p9_modes))
		return -EINVAL;

	fse->min_width = s5k3p9_modes[fse->index].width;
	fse->max_width = s5k3p9_modes[fse->index].width;
	fse->min_height = s5k3p9_modes[fse->index].height;
	fse->max_height = s5k3p9_modes[fse->index].height;

	return 0;
}

static const struct v4l2_subdev_pad_ops s5k3p9_pad_ops = {
	.enum_mbus_code = s5k3p9_enum_mbus_code,
	.enum_frame_size = s5k3p9_enum_frame_size,
	.get_fmt = s5k3p9_get_fmt,
	.set_fmt = s5k3p9_set_fmt,
};

static const struct v4l2_subdev_video_ops s5k3p9_video_ops = {
	.s_stream = s5k3p9_s_stream,
};

static const struct v4l2_subdev_ops s5k3p9_subdev_ops = {
	.pad = &s5k3p9_pad_ops,
	.video = &s5k3p9_video_ops,
};

static int s5k3p9_get_hwcfg(struct s5k3p9 *sensor)
{
	struct v4l2_fwnode_endpoint ep = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	struct device *dev = sensor->sd.dev;
	struct fwnode_handle *fwnode;
	int ret;

	fwnode = fwnode_graph_get_endpoint_by_id(dev_fwnode(dev), 0, 0, 0);
	if (!fwnode)
		return dev_err_probe(dev, -ENODEV, "no CSI2 endpoint\n");

	ret = v4l2_fwnode_endpoint_parse(fwnode, &ep);
	if (ret)
		goto out;

	if (ep.bus.mipi_csi2.num_data_lanes != S5K3P9_DATA_LANES) {
		ret = dev_err_probe(dev, -EINVAL, "expected %u lanes, got %u\n",
				    S5K3P9_DATA_LANES,
				    ep.bus.mipi_csi2.num_data_lanes);
		goto out;
	}

	ret = v4l2_link_freq_to_bitmap(dev, ep.link_frequencies,
				       ep.nr_of_link_frequencies,
				       (const s64[]){ S5K3P9_LINK_FREQ_DEFAULT },
				       1, &sensor->link_freq_bitmap);
out:
	v4l2_fwnode_endpoint_free(&ep);
	fwnode_handle_put(fwnode);
	return ret;
}

static int s5k3p9_runtime_resume(struct device *dev)
{
	return s5k3p9_power_on(to_s5k3p9(dev_get_drvdata(dev)));
}

static int s5k3p9_runtime_suspend(struct device *dev)
{
	s5k3p9_power_off(to_s5k3p9(dev_get_drvdata(dev)));
	return 0;
}

static int s5k3p9_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct s5k3p9 *sensor;
	unsigned long freq;
	u32 chip_id = 0;
	int ret, i;

	sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;

	v4l2_i2c_subdev_init(&sensor->sd, client, &s5k3p9_subdev_ops);
	sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	sensor->regmap = devm_regmap_init_i2c(client, &s5k3p9_regmap_config);
	if (IS_ERR(sensor->regmap))
		return dev_err_probe(dev, PTR_ERR(sensor->regmap),
				     "failed to init regmap\n");

	sensor->mclk = devm_v4l2_sensor_clk_get(dev, NULL);
	if (IS_ERR(sensor->mclk))
		return dev_err_probe(dev, PTR_ERR(sensor->mclk),
				     "failed to get MCLK\n");

	freq = clk_get_rate(sensor->mclk);
	if (freq != S5K3P9_MCLK_FREQ)
		return dev_err_probe(dev, -EINVAL,
				     "MCLK %lu is not %lu\n", freq,
				     S5K3P9_MCLK_FREQ);

	ret = s5k3p9_get_hwcfg(sensor);
	if (ret)
		return dev_err_probe(dev, ret, "bad hw configuration\n");

	sensor->reset_gpio = devm_gpiod_get_optional(dev, "reset",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(sensor->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(sensor->reset_gpio),
				     "failed to get reset GPIO\n");

	for (i = 0; i < ARRAY_SIZE(s5k3p9_supplies); i++) {
		sensor->supplies[i].supply = s5k3p9_supplies[i].name;
		sensor->supplies[i].init_load_uA = s5k3p9_supplies[i].ua;
	}
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(s5k3p9_supplies),
				      sensor->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
	if (ret)
		return ret;

	sensor->mode = &s5k3p9_modes[0];
	sensor->fmt.code = s5k3p9_mbus_codes[0];
	sensor->fmt.width = sensor->mode->width;
	sensor->fmt.height = sensor->mode->height;
	sensor->fmt.field = V4L2_FIELD_NONE;

	/* power once, report what the part answers, power off */
	ret = s5k3p9_power_on(sensor);
	if (ret)
		goto err_entity;

	ret = regmap_read(sensor->regmap, S5K3P9_REG_CHIP_ID, &chip_id);
	if (ret)
		dev_info(dev, "chip ID read failed (%d); the power sequence is unverified\n",
			 ret);
	else
		dev_info(dev, "chip ID 0x%02x answered at 0x%02x\n",
			 chip_id, client->addr);

	s5k3p9_power_off(sensor);

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_idle(dev);

	ret = v4l2_async_register_subdev_sensor(&sensor->sd);
	if (ret)
		goto err_pm;

	dev_info(dev, "s5k3p9 registered (skeleton: the register tables are empty)\n");
	return 0;

err_pm:
	pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);
err_entity:
	media_entity_cleanup(&sensor->sd.entity);
	return ret;
}

static void s5k3p9_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct s5k3p9 *sensor = to_s5k3p9(sd);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	pm_runtime_disable(sd->dev);
	if (!pm_runtime_status_suspended(sd->dev)) {
		s5k3p9_power_off(sensor);
		pm_runtime_set_suspended(sd->dev);
	}
}

static const struct dev_pm_ops s5k3p9_pm_ops = {
	SET_RUNTIME_PM_OPS(s5k3p9_runtime_suspend, s5k3p9_runtime_resume, NULL)
};

static const struct of_device_id s5k3p9_of_match[] = {
	{ .compatible = "samsung,s5k3p9" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, s5k3p9_of_match);

static struct i2c_driver s5k3p9_i2c_driver = {
	.driver = {
		.name = "s5k3p9",
		.pm = &s5k3p9_pm_ops,
		.of_match_table = s5k3p9_of_match,
	},
	.probe = s5k3p9_probe,
	.remove = s5k3p9_remove,
};

module_i2c_driver(s5k3p9_i2c_driver);

MODULE_DESCRIPTION("Samsung S5K3P9 sensor driver (skeleton)");
MODULE_LICENSE("GPL");
