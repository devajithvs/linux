// SPDX-License-Identifier: GPL-2.0
/*
 * Sony IMX882 (LYT-600) sensor driver, rear main camera of the OnePlus Nord 4.
 *
 * The part sits on CCI0 master 0 at 0x1a, behind CSIPHY0 wired as three C-PHY
 * trios, with MCLK0 at 24 MHz on TLMM 64 and reset on TLMM 15. There is no
 * public register programme for this module, so every table here is the
 * vendor stack's own write sequence captured from the stock camera log and
 * turned into a programme by results/device/2026-11-25-imx882-survey/ (F0508).
 * The tables live in imx882-regs.h and are written in acquisition order:
 * init -> QSC -> mode with group hold -> SRAM patches -> first AE -> stream on.
 *
 * Only the stock 4096x3072 RAW10 30 fps mode is carried (res index 1).
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#include <media/media-entity.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#include "imx882-regs.h"

#define IMX882_MCLK_FREQ		(24000000UL)

#define IMX882_REG_CHIP_ID_MSB		0x0016
#define IMX882_REG_CHIP_ID_LSB		0x0017
#define IMX882_CHIP_ID			0x8202

#define IMX882_REG_MODE_SELECT		0x0100
#define IMX882_MODE_STANDBY		0x00
#define IMX882_MODE_STREAMING		0x01
#define IMX882_REG_GROUP_HOLD		0x0104

#define IMX882_WIDTH			4096
#define IMX882_HEIGHT			3072
#define IMX882_DATA_LANES		3	/* C-PHY trios on CSIPHY0 */
#define IMX882_LINK_FREQ		(4580566666LL)
#define IMX882_PIXEL_RATE		(377487360LL)	/* 4096*3072*30 */

#define IMX882_QSC_BASE			0xc000
#define IMX882_SRAM0_BASE		0xd200
#define IMX882_SRAM1_BASE		0xd300

struct imx882_supply {
	const char *name;
	unsigned int ua;
};

static const struct imx882_supply imx882_supplies[] = {
	{ "avdd", 60000 },
	{ "dvdd", 200000 },
	{ "dovdd", 1000 },
};

static const s64 imx882_link_freq_menu[] = {
	IMX882_LINK_FREQ,
};

static const struct regmap_config imx882_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.max_register = 0xffff,
};

struct imx882_mode {
	u32 width;
	u32 height;
	const struct imx882_reg *regs;
	unsigned int num_regs;
};

static const struct imx882_mode imx882_modes[] = {
	{
		.width = IMX882_WIDTH,
		.height = IMX882_HEIGHT,
		.regs = imx882_mode_4096x3072_30_regs,
		.num_regs = ARRAY_SIZE(imx882_mode_4096x3072_30_regs),
	},
};

/*
 * The 4096x3072 programme this driver streams arrives at the VFE as BGGR:
 * F0531's attended four-phase comparison of the rear main is the evidence,
 * and F0533's 10-bit sample of the same hand is warm only in the BGGR phase.
 */
static const u32 imx882_mbus_codes[] = {
	MEDIA_BUS_FMT_SBGGR10_1X10,
};

struct imx882 {
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct regmap *regmap;
	struct clk *mclk;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[ARRAY_SIZE(imx882_supplies)];
	struct v4l2_ctrl_handler ctrl_handler;
	struct mutex lock;	/* serialises power and streaming */

	const struct imx882_mode *mode;
	struct v4l2_mbus_framefmt fmt;
	unsigned long link_freq_bitmap;
	bool powered;
	bool streaming;
};

static inline struct imx882 *to_imx882(struct v4l2_subdev *sd)
{
	return container_of(sd, struct imx882, sd);
}

static int imx882_write_table(struct imx882 *sensor,
			      const struct imx882_reg *regs, unsigned int n)
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
	msleep(20);

	sensor->powered = true;

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
	sensor->powered = false;
}

static void imx882_log_ids(struct imx882 *sensor, const char *what)
{
	unsigned int msb = 0, lsb = 0;
	int ret;

	ret = regmap_read(sensor->regmap, IMX882_REG_CHIP_ID_MSB, &msb);
	if (!ret)
		ret = regmap_read(sensor->regmap, IMX882_REG_CHIP_ID_LSB, &lsb);
	if (ret) {
		dev_warn(sensor->sd.dev, "%s: chip ID read failed (%d)\n",
			 what, ret);
		return;
	}

	dev_info(sensor->sd.dev, "%s: chip ID 0x%04x (want 0x%04x)\n",
		 what, (msb << 8) | lsb, IMX882_CHIP_ID);
}

static int imx882_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct imx882 *sensor = to_imx882(sd);
	unsigned int mode = 0;
	int ret;

	mutex_lock(&sensor->lock);

	if (!enable) {
		if (sensor->streaming) {
			ret = regmap_write(sensor->regmap, IMX882_REG_MODE_SELECT,
					   IMX882_MODE_STANDBY);
			if (ret)
				dev_warn(sd->dev, "stream off failed (%d)\n", ret);
			sensor->streaming = false;
		}
		if (sensor->powered)
			imx882_power_off(sensor);
		mutex_unlock(&sensor->lock);
		return 0;
	}

	if (sensor->streaming) {
		mutex_unlock(&sensor->lock);
		return 0;
	}

	if (!sensor->powered) {
		ret = imx882_power_on(sensor);
		if (ret) {
			mutex_unlock(&sensor->lock);
			return ret;
		}
	}

	imx882_log_ids(sensor, "stream on");

	/* T0: power-up/init */
	ret = imx882_write_table(sensor, imx882_init_regs,
				 ARRAY_SIZE(imx882_init_regs));
	if (ret)
		goto err_power;

	/* T1: the 3072-byte QSC correction blob, sequential from 0xc000 */
	ret = regmap_bulk_write(sensor->regmap, IMX882_QSC_BASE,
				imx882_qsc_blob, sizeof(imx882_qsc_blob));
	if (ret)
		goto err_power;

	/* T4-T6: mode table under group hold */
	ret = regmap_write(sensor->regmap, IMX882_REG_GROUP_HOLD, 1);
	if (!ret)
		ret = imx882_write_table(sensor, sensor->mode->regs,
					 sensor->mode->num_regs);
	if (!ret)
		ret = regmap_write(sensor->regmap, IMX882_REG_GROUP_HOLD, 0);
	if (ret)
		goto err_power;

	/* T7-T9: SRAM preamble under group hold */
	ret = regmap_write(sensor->regmap, IMX882_REG_GROUP_HOLD, 1);
	if (!ret)
		ret = imx882_write_table(sensor, imx882_sram_preamble_regs,
					 ARRAY_SIZE(imx882_sram_preamble_regs));
	if (!ret)
		ret = regmap_write(sensor->regmap, IMX882_REG_GROUP_HOLD, 0);
	if (ret)
		goto err_power;

	/* T10/T11: the two 192-byte SRAM patches */
	ret = regmap_bulk_write(sensor->regmap, IMX882_SRAM0_BASE,
				imx882_sram_0xd200, sizeof(imx882_sram_0xd200));
	if (!ret)
		ret = regmap_bulk_write(sensor->regmap, IMX882_SRAM1_BASE,
					imx882_sram_0xd300,
					sizeof(imx882_sram_0xd300));
	if (ret)
		goto err_power;

	/* T15: first AE apply, then the frame header block */
	ret = imx882_write_table(sensor, imx882_first_ae_regs,
				 ARRAY_SIZE(imx882_first_ae_regs));
	if (ret)
		goto err_power;

	/* T16: stream on */
	ret = regmap_write(sensor->regmap, IMX882_REG_MODE_SELECT,
			   IMX882_MODE_STREAMING);
	if (ret)
		goto err_power;

	sensor->streaming = true;

	/* a frame period at 30 fps, then report what the part holds */
	msleep(50);
	if (!regmap_read(sensor->regmap, IMX882_REG_MODE_SELECT, &mode))
		dev_info(sd->dev, "stream on: 0x0100 = 0x%02x\n", mode);

	mutex_unlock(&sensor->lock);
	return 0;

err_power:
	imx882_power_off(sensor);
	mutex_unlock(&sensor->lock);
	return ret;
}

static int imx882_get_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *format)
{
	struct imx882 *sensor = to_imx882(sd);

	format->format = sensor->fmt;
	return 0;
}

static int imx882_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *format)
{
	struct imx882 *sensor = to_imx882(sd);
	struct v4l2_mbus_framefmt *fmt = &format->format;

	fmt->code = imx882_mbus_codes[0];
	fmt->width = clamp(fmt->width, 1U, IMX882_WIDTH);
	fmt->height = clamp(fmt->height, 1U, IMX882_HEIGHT);
	fmt->field = V4L2_FIELD_NONE;

	if (format->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		sensor->fmt = *fmt;

	return 0;
}

static int imx882_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index >= ARRAY_SIZE(imx882_mbus_codes))
		return -EINVAL;

	code->code = imx882_mbus_codes[code->index];
	return 0;
}

static int imx882_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index >= ARRAY_SIZE(imx882_modes))
		return -EINVAL;

	fse->min_width = imx882_modes[fse->index].width;
	fse->max_width = imx882_modes[fse->index].width;
	fse->min_height = imx882_modes[fse->index].height;
	fse->max_height = imx882_modes[fse->index].height;

	return 0;
}

static const struct v4l2_subdev_pad_ops imx882_pad_ops = {
	.enum_mbus_code = imx882_enum_mbus_code,
	.enum_frame_size = imx882_enum_frame_size,
	.get_fmt = imx882_get_fmt,
	.set_fmt = imx882_set_fmt,
};

static const struct v4l2_subdev_video_ops imx882_video_ops = {
	.s_stream = imx882_s_stream,
};

static const struct v4l2_subdev_ops imx882_subdev_ops = {
	.pad = &imx882_pad_ops,
	.video = &imx882_video_ops,
};

static int imx882_get_hwcfg(struct imx882 *sensor)
{
	struct v4l2_fwnode_endpoint ep = {
		.bus_type = V4L2_MBUS_CSI2_CPHY,
	};
	struct device *dev = sensor->sd.dev;
	struct fwnode_handle *fwnode;
	int ret;

	fwnode = fwnode_graph_get_endpoint_by_id(dev_fwnode(dev), 0, 0, 0);
	if (!fwnode)
		return dev_err_probe(dev, -ENODEV, "no CSI2 endpoint\n");

	ret = v4l2_fwnode_endpoint_alloc_parse(fwnode, &ep);
	if (ret)
		goto out;

	if (ep.bus.mipi_csi2.num_data_lanes != IMX882_DATA_LANES) {
		ret = dev_err_probe(dev, -EINVAL, "expected %u trios, got %u\n",
				    IMX882_DATA_LANES,
				    ep.bus.mipi_csi2.num_data_lanes);
		goto out;
	}

	ret = v4l2_link_freq_to_bitmap(dev, ep.link_frequencies,
				       ep.nr_of_link_frequencies,
				       imx882_link_freq_menu,
				       ARRAY_SIZE(imx882_link_freq_menu),
				       &sensor->link_freq_bitmap);
out:
	v4l2_fwnode_endpoint_free(&ep);
	fwnode_handle_put(fwnode);
	return ret;
}

static int imx882_init_controls(struct imx882 *sensor)
{
	struct v4l2_ctrl_handler *hdl = &sensor->ctrl_handler;
	struct v4l2_ctrl *ctrl;
	int ret;

	ret = v4l2_ctrl_handler_init(hdl, 2);
	if (ret)
		return ret;

	sensor->sd.ctrl_handler = hdl;

	ctrl = v4l2_ctrl_new_int_menu(hdl, NULL, V4L2_CID_LINK_FREQ,
				      ARRAY_SIZE(imx882_link_freq_menu) - 1, 0,
				      imx882_link_freq_menu);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	/* CAMSS reads this to derive the pixel and VFE clocks */
	v4l2_ctrl_new_std(hdl, NULL, V4L2_CID_PIXEL_RATE, IMX882_PIXEL_RATE,
			  IMX882_PIXEL_RATE, 1, IMX882_PIXEL_RATE);

	if (!hdl->error)
		return 0;

	ret = hdl->error;
	v4l2_ctrl_handler_free(hdl);
	sensor->sd.ctrl_handler = NULL;
	return ret;
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
		return dev_err_probe(dev, -EINVAL, "MCLK %lu is not %lu\n",
				     freq, IMX882_MCLK_FREQ);

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

	ret = imx882_get_hwcfg(sensor);
	if (ret)
		return ret;

	ret = imx882_init_controls(sensor);
	if (ret)
		return ret;

	mutex_init(&sensor->lock);

	sensor->mode = &imx882_modes[0];
	sensor->fmt.code = imx882_mbus_codes[0];
	sensor->fmt.width = sensor->mode->width;
	sensor->fmt.height = sensor->mode->height;
	sensor->fmt.field = V4L2_FIELD_NONE;

	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
	if (ret)
		goto err_ctrl;

	ret = v4l2_async_register_subdev_sensor(&sensor->sd);
	if (ret)
		goto err_entity;

	dev_info(dev, "imx882 registered: %ux%u RAW10, C-PHY 3 trios\n",
		 sensor->mode->width, sensor->mode->height);
	return 0;

err_entity:
	media_entity_cleanup(&sensor->sd.entity);
err_ctrl:
	v4l2_ctrl_handler_free(&sensor->ctrl_handler);
	return ret;
}

static void imx882_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx882 *sensor = to_imx882(sd);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&sensor->ctrl_handler);
	mutex_destroy(&sensor->lock);
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

MODULE_DESCRIPTION("Sony IMX882 camera sensor driver");
MODULE_LICENSE("GPL");
