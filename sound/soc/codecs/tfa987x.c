// SPDX-License-Identifier: GPL-2.0-only
/*
 * NXP TFA987x speaker amplifier.
 *
 * The TFA9873/TFA9874 have no mainline driver; the phone's vendor stack builds
 * NXP's tfa98xx v6 driver as an out-of-tree module. This driver covers the
 * part of that module this board needs to bring the amplifier up and identify
 * it: reset, the revision register and an ASoC playback DAI, so the machine
 * driver can link the amplifier into the sound card.  The revision register's
 * low byte names the part: 0x73 is a TFA9873 and 0x74 a TFA9874
 * (tfa98xx_v6.c:6084-6108).
 *
 * The vendor driver cannot play without the DSP container file
 * (tfa98xx.cnt): its ASoC startup refuses while the container has not
 * loaded, and the cold start walks the container's device list. The
 * container itself is a binary this driver does not carry and the board's
 * copy lives on the phone's odm firmware; until it is loaded and the vendor
 * DSP HAL is ported, the DAI reports -EOPNOTSUPP instead of producing
 * silence that looks like success.
 *
 * Register access is 8-bit address, 16-bit big-endian value, as the vendor
 * driver configures it (tfa98xx_v6.c:5503-5512).
 */

#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <sound/soc.h>

#define TFA987X_REG_REVISION	0x03
#define TFA9873_ID		0x73
#define TFA9874_ID		0x74

#define TFA987X_RESET_PULSE_US	5000

struct tfa987x_priv {
	struct device *dev;
	struct regmap *regmap;
};

static const struct regmap_config tfa987x_regmap_config = {
	.reg_bits = 8,
	.val_bits = 16,
	.val_format_endian = REGMAP_ENDIAN_BIG,
	.max_register = 0xff,
	.cache_type = REGCACHE_NONE,
};

/*
 * The vendor driver pulses the reset line high for 5 ms and then low for
 * 5 ms (tfa98xx_v6.c:5539-5546, :6060) with the line described as active
 * high (avalon-24211-audio-cliffs-overlay.dtsi:80-102, flag 0), so the DT
 * uses GPIO_ACTIVE_HIGH.
 */
static int tfa987x_reset(struct device *dev, struct gpio_desc *reset)
{
	gpiod_set_value_cansleep(reset, 1);
	usleep_range(TFA987X_RESET_PULSE_US, TFA987X_RESET_PULSE_US + 1000);
	gpiod_set_value_cansleep(reset, 0);
	usleep_range(TFA987X_RESET_PULSE_US, TFA987X_RESET_PULSE_US + 1000);

	return 0;
}

static int tfa987x_dai_startup(struct snd_pcm_substream *substream,
			       struct snd_soc_dai *dai)
{
	/*
	 * No DSP container has been loaded, so the amplifier is still in
	 * standby and would drop whatever I2S data the LPASS sends it.
	 */
	dev_warn_once(dai->dev, "playback needs the DSP container; not wired up\n");

	return -EOPNOTSUPP;
}

static const struct snd_soc_dai_ops tfa987x_dai_ops = {
	.startup = tfa987x_dai_startup,
};

static struct snd_soc_dai_driver tfa987x_dai_driver[] = {
	{
		.name = "tfa987x-aif",
		.playback = {
			.stream_name = "Speaker Playback",
			.formats = SNDRV_PCM_FMTBIT_S16_LE |
				   SNDRV_PCM_FMTBIT_S24_LE |
				   SNDRV_PCM_FMTBIT_S32_LE,
			.rates = SNDRV_PCM_RATE_8000_192000,
			.rate_min = 8000,
			.rate_max = 192000,
			.channels_min = 1,
			.channels_max = 2,
		},
		.ops = &tfa987x_dai_ops,
	},
};

static const struct snd_soc_component_driver tfa987x_component_driver = {
	.name = "tfa987x",
};

static int tfa987x_i2c_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct tfa987x_priv *priv;
	struct gpio_desc *reset;
	unsigned int rev;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	i2c_set_clientdata(client, priv);

	priv->regmap = devm_regmap_init_i2c(client, &tfa987x_regmap_config);
	if (IS_ERR(priv->regmap))
		return dev_err_probe(dev, PTR_ERR(priv->regmap),
				     "failed to allocate the regmap\n");

	ret = devm_regulator_get_enable(dev, "vddd");
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable the amplifier supply\n");

	reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(reset))
		return dev_err_probe(dev, PTR_ERR(reset),
				     "failed to get the reset line\n");

	tfa987x_reset(dev, reset);

	ret = regmap_read(priv->regmap, TFA987X_REG_REVISION, &rev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read the revision\n");

	switch (rev & 0xff) {
	case TFA9873_ID:
		dev_info(dev, "TFA9873 revision 0x%04x\n", rev);
		break;
	case TFA9874_ID:
		dev_info(dev, "TFA9874 revision 0x%04x\n", rev);
		break;
	default:
		dev_err(dev, "unexpected revision 0x%04x\n", rev);
		return -ENODEV;
	}

	return devm_snd_soc_register_component(dev, &tfa987x_component_driver,
					       tfa987x_dai_driver,
					       ARRAY_SIZE(tfa987x_dai_driver));
}

static const struct of_device_id tfa987x_of_match[] = {
	{ .compatible = "nxp,tfa9873" },
	{ .compatible = "nxp,tfa9874" },
	{ }
};
MODULE_DEVICE_TABLE(of, tfa987x_of_match);

static const struct i2c_device_id tfa987x_i2c_id[] = {
	{ "tfa987x", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, tfa987x_i2c_id);

static struct i2c_driver tfa987x_i2c_driver = {
	.driver = {
		.name = "tfa987x",
		.of_match_table = tfa987x_of_match,
	},
	.probe = tfa987x_i2c_probe,
	.id_table = tfa987x_i2c_id,
};
module_i2c_driver(tfa987x_i2c_driver);

MODULE_DESCRIPTION("NXP TFA987x speaker amplifier");
MODULE_LICENSE("GPL");
