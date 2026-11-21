// SPDX-License-Identifier: GPL-2.0-only
/*
 * OnePlus three-position alert slider on an AKM AK09970 hall sensor.
 *
 * The slider is a magnet that moves past a 3-axis hall sensor wired to an
 * I2C bus. Each of the three detents holds the magnet at a different place
 * along one sensor axis, so a raw reading of that axis identifies the
 * position. The driver reports it on the input ABS_SND_PROFILE axis with
 * SND_PROFILE_SILENT, SND_PROFILE_VIBRATE and SND_PROFILE_RING, the
 * convention the OnePlus 6 slider uses (arch/arm64/boot/dts/qcom/
 * sdm845-oneplus-common.dtsi, gpio-keys with linux,input-type = <EV_ABS>).
 *
 * The sensor is polled: one single measurement per period keeps the average
 * current in the microamp range, and a slider does not need an interrupt.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#define AK09970_REG_WIA		0x00
#define AK09970_REG_DATA	0x17
#define AK09970_REG_CNTL2	0x21
#define AK09970_REG_SRST	0x30

#define AK09970_WIA_COMPANY	0x48
#define AK09970_WIA_DEVICE	0xc1

#define AK09970_CNTL2_SINGLE	0x21	/* SMR=1 (wide range) | MODE=1 (single) */

#define SLIDER_POLL_MS		100
#define SLIDER_SAMPLES		4
#define SLIDER_DEBOUNCE		3
#define SLIDER_STABLE		2500
#define SLIDER_HYSTERESIS	400

struct slider_priv {
	struct i2c_client *client;
	struct input_dev *input;
	struct delayed_work work;
	u8 axis;
	s32 weights[3];
	s32 history[3];
	int history_count;
	s32 band_low_max;
	s32 band_mid_max;
	u8 position_map[3];
	int band;
	bool logged_read;
	unsigned int cycles;
	s32 last_value;
	s32 last_x, last_y, last_z;
	int position;
	int candidate;
	int candidate_count;
};

static int ak09970_read(struct i2c_client *client, u8 reg, u8 *buf, int len)
{
	struct i2c_msg msgs[2] = {
		{ .addr = client->addr, .flags = 0, .len = 1, .buf = &reg },
		{ .addr = client->addr, .flags = I2C_M_RD, .len = len, .buf = buf },
	};
	int ret, i;

	for (i = 0; i < 3; i++) {
		ret = i2c_transfer(client->adapter, msgs, 2);
		if (ret == 2)
			return 0;
		usleep_range(1000, 2000);
	}

	return ret < 0 ? ret : -EIO;
}

static int ak09970_write(struct i2c_client *client, u8 reg, u8 val)
{
	u8 buf[2] = { reg, val };
	struct i2c_msg msg = { .addr = client->addr, .flags = 0,
			       .len = 2, .buf = buf };
	int ret, i;

	for (i = 0; i < 3; i++) {
		ret = i2c_transfer(client->adapter, &msg, 1);
		if (ret == 1)
			return 0;
		usleep_range(1000, 2000);
	}

	return ret < 0 ? ret : -EIO;
}

/*
 * One single measurement. The sensor latches ST, HZ, HY and HX in one
 * 8-byte block at 0x17 (AK09970N datasheet 12.2, read start address 17h:
 * ST[15:8] ST[7:0] HZ[15:8] HZ[7:0] HY[15:8] HY[7:0] HX[15:8] HX[7:0]).
 */
static int ak09970_measure(struct slider_priv *priv, s32 *value)
{
	struct i2c_client *client = priv->client;
	struct device *dev = &client->dev;
	u8 buf[7];
	int ret;
	s16 x, y, z;

	ret = ak09970_write(client, AK09970_REG_CNTL2, AK09970_CNTL2_SINGLE);
	if (ret) {
		/*
		 * The chip can come out of the early-boot failed reset in a
		 * state where it ignores the mode write. One reset recovers it.
		 */
		dev_warn_ratelimited(dev, "measure write failed (%d); resetting\n", ret);
		ak09970_write(client, AK09970_REG_SRST, 0x01);
		msleep(5);
		ret = ak09970_write(client, AK09970_REG_CNTL2, AK09970_CNTL2_SINGLE);
		if (ret)
			return ret;
	}

	/* TSM is 0.872 ms in the low-noise setting. */
	usleep_range(2000, 3000);

	ret = ak09970_read(client, AK09970_REG_DATA, buf, sizeof(buf));
	if (ret) {
		dev_warn_ratelimited(dev, "measure read failed (%d)\n", ret);
		return ret;
	}

	/*
	 * The data block at 0x17 is 7 bytes: one status byte, then HZ, HY and
	 * HX, each 16-bit big-endian (the vendor driver reads exactly 7 and
	 * uses the same fields).
	 */
	x = (s16)((buf[5] << 8) | buf[6]);
	y = (s16)((buf[3] << 8) | buf[4]);
	z = (s16)((buf[1] << 8) | buf[2]);

	*value = priv->weights[0] * x + priv->weights[1] * y + priv->weights[2] * z;
	priv->last_x = x;
	priv->last_y = y;
	priv->last_z = z;

	if (!priv->logged_read) {
		dev_dbg(dev, "first sample x=%d y=%d z=%d v=%d\n", x, y, z, *value);
		priv->logged_read = true;
	}

	return 0;
}

static int slider_band(struct slider_priv *priv, s32 value)
{
	if (value < priv->band_low_max)
		return 0;
	if (value < priv->band_mid_max)
		return 1;
	return 2;
}

/*
 * The bands are per board: the magnet does not have to cross the raw bands in
 * the order top, middle, bottom. The DT maps each raw band to its SND_PROFILE
 * value; a reading inside SLIDER_HYSTERESIS of a boundary keeps the band the
 * slider is already in.
 */
static int slider_band_hysteresis(struct slider_priv *priv, s32 value)
{
	int band = slider_band(priv, value);

	if (priv->band < 0)
		return band;

	if (band != priv->band &&
	    ((value > priv->band_low_max - SLIDER_HYSTERESIS &&
	      value < priv->band_low_max + SLIDER_HYSTERESIS) ||
	     (value > priv->band_mid_max - SLIDER_HYSTERESIS &&
	      value < priv->band_mid_max + SLIDER_HYSTERESIS)))
		band = priv->band;

	return band;
}

static void slider_work(struct work_struct *work)
{
	struct slider_priv *priv = container_of(to_delayed_work(work),
						struct slider_priv, work);
	s32 sum = 0;
	s32 value, hi, lo;
	bool stable;
	int i, ret, band, pos;

	if (++priv->cycles % 10 == 0)
		dev_dbg(&priv->client->dev,
			"sample x=%d y=%d z=%d v=%d band=%d pos=%d\n",
			priv->last_x, priv->last_y, priv->last_z,
			priv->last_value, priv->band, priv->position);

	for (i = 0; i < SLIDER_SAMPLES; i++) {
		s32 sample;

		ret = ak09970_measure(priv, &sample);
		if (ret) {
			dev_dbg(&priv->client->dev, "measurement failed: %d\n", ret);
			goto out;
		}
		sum += sample;
	}
	value = sum / SLIDER_SAMPLES;
	priv->last_value = value;

	/*
	 * A transit through a band lasts a few 100 ms and moves the reading by
	 * thousands per sample; a detent holds. Only accept a band while the
	 * last three averaged readings are close, so a flick cannot report the
	 * detent it passes through.
	 */
	priv->history[2] = priv->history[1];
	priv->history[1] = priv->history[0];
	priv->history[0] = value;
	if (priv->history_count < 3)
		priv->history_count++;
	hi = max(priv->history[0], max(priv->history[1], priv->history[2]));
	lo = min(priv->history[0], min(priv->history[1], priv->history[2]));
	stable = priv->history_count >= 3 && hi - lo < SLIDER_STABLE;

	band = slider_band_hysteresis(priv, value);
	if (stable && band != priv->band) {
		if (band == priv->candidate) {
			priv->candidate_count++;
		} else {
			priv->candidate = band;
			priv->candidate_count = 1;
		}

		if (priv->candidate_count >= SLIDER_DEBOUNCE) {
			priv->band = band;
			priv->candidate_count = 0;
			pos = priv->position_map[band];
			if (pos != priv->position) {
				priv->position = pos;
				input_report_abs(priv->input, ABS_SND_PROFILE, pos);
				input_sync(priv->input);
			}
		}
	} else {
		priv->candidate_count = 0;
	}

out:
	schedule_delayed_work(&priv->work, msecs_to_jiffies(SLIDER_POLL_MS));
}

static void slider_cancel_work(void *data)
{
	struct slider_priv *priv = data;

	cancel_delayed_work_sync(&priv->work);
}

static int slider_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct slider_priv *priv;
	struct input_dev *input;
	const char *axis;
	u32 thresholds[2];
	u32 map[3];
	int i;
	u8 wia[4];
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->client = client;
	i2c_set_clientdata(client, priv);

	ret = devm_regulator_get_enable(dev, "vcc");
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable vcc\n");

	/*
	 * The sensor's rail is left on by the bootloader; the chip does not
	 * answer this early in the boot on this board, so a bus error here is
	 * not fatal: the poll below retries until it does. A device that does
	 * answer with a different id is a real mismatch and stays fatal.
	 */
	ret = ak09970_write(client, AK09970_REG_SRST, 0x01);
	if (ret) {
		dev_info(dev, "sensor not ready yet (%d); the poll will retry\n", ret);
	} else {
		msleep(5);

		ret = ak09970_read(client, AK09970_REG_WIA, wia, sizeof(wia));
		if (ret) {
			dev_info(dev, "device id not readable yet (%d)\n", ret);
		} else if (wia[0] != AK09970_WIA_COMPANY ||
			   (wia[1] != AK09970_WIA_DEVICE &&
			    wia[1] != AK09970_WIA_DEVICE - 1)) {
			return dev_err_probe(dev, -ENODEV,
					     "unexpected device id %02x%02x\n",
					     wia[0], wia[1]);
		}
	}

	ret = device_property_read_string(dev, "oneplus,axis", &axis);
	if (ret)
		return dev_err_probe(dev, ret, "missing oneplus,axis\n");
	if (!strcmp(axis, "x"))
		priv->axis = 0;
	else if (!strcmp(axis, "y"))
		priv->axis = 1;
	else if (!strcmp(axis, "z"))
		priv->axis = 2;
	else
		return dev_err_probe(dev, -EINVAL, "bad oneplus,axis %s\n", axis);

	priv->weights[priv->axis] = 1;
	ret = device_property_read_u32_array(dev, "oneplus,axis-weights", map, 3);
	if (!ret) {
		for (i = 0; i < 3; i++)
			priv->weights[i] = (s32)map[i];
	}

	ret = device_property_read_u32_array(dev, "oneplus,position-thresholds",
					     thresholds, 2);
	if (ret)
		return dev_err_probe(dev, ret, "missing oneplus,position-thresholds\n");
	priv->band_low_max = (s32)thresholds[0];
	priv->band_mid_max = (s32)thresholds[1];
	if (priv->band_low_max >= priv->band_mid_max)
		return dev_err_probe(dev, -EINVAL,
				     "oneplus,position-thresholds are not ascending\n");

	map[0] = SND_PROFILE_SILENT;
	map[1] = SND_PROFILE_VIBRATE;
	map[2] = SND_PROFILE_RING;
	ret = device_property_read_u32_array(dev, "oneplus,position-map", map, 3);
	if (ret && ret != -EINVAL)
		return dev_err_probe(dev, ret, "bad oneplus,position-map\n");
	for (ret = 0; ret < 3; ret++) {
		if (map[ret] > SND_PROFILE_RING)
			return dev_err_probe(dev, -EINVAL,
					     "oneplus,position-map value out of range\n");
		priv->position_map[ret] = map[ret];
	}

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;

	input->name = "OnePlus alert slider";
	input->phys = dev_name(dev);
	input->id.bustype = BUS_I2C;

	input_set_abs_params(input, ABS_SND_PROFILE, SND_PROFILE_SILENT,
			     SND_PROFILE_RING, 0, 0);

	priv->input = input;
	priv->band = -1;
	priv->position = -1;

	ret = input_register_device(input);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register the input device\n");

	INIT_DELAYED_WORK(&priv->work, slider_work);
	ret = devm_add_action_or_reset(dev, slider_cancel_work, priv);
	if (ret)
		return dev_err_probe(dev, ret, "failed to set up the poll work\n");
	schedule_delayed_work(&priv->work, 0);

	return 0;
}

static const struct of_device_id slider_of_match[] = {
	{ .compatible = "oneplus,alert-slider-ak09970" },
	{ }
};
MODULE_DEVICE_TABLE(of, slider_of_match);

static const struct i2c_device_id slider_id[] = {
	{ "alert-slider-ak09970" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, slider_id);

static struct i2c_driver slider_driver = {
	.driver = {
		.name = "oneplus-alert-slider",
		.of_match_table = slider_of_match,
	},
	.probe = slider_probe,
	.id_table = slider_id,
};
module_i2c_driver(slider_driver);

MODULE_DESCRIPTION("OnePlus alert slider on an AK09970 hall sensor");
MODULE_LICENSE("GPL");
