// SPDX-License-Identifier: GPL-2.0-only
/*
 * NXP TFA9873/TFA9874 speaker amplifier, brought up from NXP's own
 * configuration container.
 *
 * The part is a "probus" device: no internal DSP, so everything the vendor's
 * tfa98xx module would load into a DSP (the patch, the algorithm parameters,
 * the volume table) is either absent from the phone's own build or is a
 * register list inside the container file.  The phone's stock
 * oplus_audio_tfa98xx_v6.ko proves it: its tfa98xx_write_dsp() frees the
 * message and returns 0, and its tfa98xx_read_dsp() is `mov w0, wzr; ret`.
 * What is left is what this driver does: read /lib/firmware/tfa98xx.cnt,
 * find this amplifier's device list by its I2C address, and write that list
 * and the selected profile's register/bitfield items over I2C, then take the
 * device out of powerdown.
 *
 * The container is the authority on the digital interface.  Its device list
 * for this board (OPPO / TFA / 9873N1B, 5,822 B, ODM's
 * /odm/firmware/tfa98xx.cnt) programs TDM: TDMNBCK 2, TDMSLOTS 1 (two slots
 * per frame), TDMSLLN 31 and TDMSSIZE 31 (32-bit slots), AUDFS 8 (48 kHz),
 * with 0x34 taking slot 0 and 0x35 slot 1.  Two 32-bit slots at 48 kHz is
 * exactly what a 2-channel, 32-bit I2S frame is, so the card's speaker link
 * runs S32_LE (sound/soc/qcom/sc8280xp.c's BE fixup) and this driver only
 * checks that the stream's rate has a profile.
 *
 * Register access is 8-bit address, 16-bit big-endian value, as the vendor
 * driver configures it (tfa98xx_v6.c:5503-5512).  Bitfields are the
 * datasheet's (reg << 8 | pos << 4 | (len - 1)), taken from
 * tfa9873_tfafieldnames.h.
 */

#include <linux/crc32.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/unaligned.h>
#include <linux/string.h>
#include <sound/soc.h>

#define TFA987X_REG_STATUS1	0x10
#define TFA987X_REG_STATUS2	0x11
#define TFA987X_REG_REVISION	0x03
#define TFA9873_ID		0x73
#define TFA9874_ID		0x74

#define TFA987X_RESET_PULSE_US	5000
#define TFA987X_CLK_TRIES	50
#define TFA987X_CLK_POLL_MS	10

/* Bitfields: tfa9873_tfafieldnames.h. */
#define BF_PWDN		0x0000	/* powerdown selection */
#define BF_I2CR		0x0010	/* I2C reset, auto clear */
#define BF_AMPE		0x0030	/* activate amplifier */
#define BF_DCA		0x0040	/* activate DC-to-DC converter */
#define BF_MANSCONF	0x0120	/* I2C configured: leave wait4srcsettings */
#define BF_AUDFS	0x0203	/* sample rate (fs) */
#define BF_OTDS		0x1020	/* OTP alarm (sticky) */
#define BF_OCDS		0x1030	/* OCP amplifier (sticky) */
#define BF_UVDS		0x1040	/* UVP alarm (sticky) */
#define BF_MANALARM	0x1050	/* alarm state */
#define BF_CLKS		0x1060	/* clocks stable */
#define BF_NOCLK	0x1080	/* lost clock (sticky) */
#define BF_TDMERR	0x10a0	/* TDM error */
#define BF_DCIL		0x1100	/* DCDC current limiting */
#define BF_DCDCA	0x1110	/* DCDC active */
#define BF_PLLS		0x1160	/* PLL lock */
#define BF_TDMLUTER	0x1180	/* TDM LUT error */
#define BF_SWS		0x11d0	/* amplifier engage */
#define BF_AMPS		0x11e0	/* amplifier enable */
#define BF_AREFS	0x11f0	/* references enable */

#define BF_REG(f)	((f) >> 8)
#define BF_POS(f)	(((f) >> 4) & 0xf)
#define BF_LEN(f)	(((f) & 0xf) + 1)

/* Descriptor types: nxpTfaDescriptorType_t, tfa98xx_parameters.h:581-598. */
#define DSC_DEVICE	0
#define DSC_PROFILE	1
#define DSC_REGISTER	2
#define DSC_STRING	3
#define DSC_FILE	4
#define DSC_PATCH	5
#define DSC_MARKER	6
#define DSC_BITFIELD	16
#define DSC_DEFAULT	17
#define DSC_NO_INIT	24

#define TFA_CNT_HDR_LEN		46	/* through nliveData; index[] follows */
#define TFA_CNT_CRCOFF		0x0a

static int rate_to_audfs(unsigned int rate)
{
	switch (rate) {
	case 8000:	return 0;
	case 11025:	return 1;
	case 12000:	return 2;
	case 16000:	return 3;
	case 22050:	return 4;
	case 24000:	return 5;
	case 32000:	return 6;
	case 44100:	return 7;
	case 48000:	return 8;
	case 96000:	return 9;
	default:	return -EINVAL;
	}
}

/**
 * struct tfa987x_priv - one amplifier
 * @dev: this amplifier's device
 * @regmap: the I2C register map
 * @fw: the container as handed over by the firmware loader
 * @addr: I2C address, the key into the container's device lists
 * @dev_off: offset of this amplifier's device list inside the container
 * @lock: serialises the register sequences against the DAI callbacks
 * @profile: index of the selected profile within the device list
 * @configured: the device list and the profile have been written
 * @rate: the stream's rate, for the log and the profile match
 */
struct tfa987x_priv {
	struct device *dev;
	struct regmap *regmap;
	const struct firmware *fw;
	unsigned int addr;
	size_t dev_off;
	struct mutex lock;
	int profile;
	bool configured;
	bool running;
	unsigned int rate;
};

static const struct regmap_config tfa987x_regmap_config = {
	.reg_bits = 8,
	.val_bits = 16,
	.val_format_endian = REGMAP_ENDIAN_BIG,
	.max_register = 0xff,
	.cache_type = REGCACHE_NONE,
};

/* --- container access, all bounds-checked against the firmware blob ------- */

static bool cnt_has(struct tfa987x_priv *priv, size_t off, size_t size)
{
	return off <= priv->fw->size && size <= priv->fw->size - off;
}

static int cnt_desc(struct tfa987x_priv *priv, size_t off, size_t *doff,
		    u8 *type)
{
	u32 v;

	if (!cnt_has(priv, off, 4))
		return -EINVAL;

	v = get_unaligned_le32(priv->fw->data + off);
	*doff = v & 0xffffff;
	*type = v >> 24;
	if (*doff >= priv->fw->size)
		return -EINVAL;

	return 0;
}

static const char *cnt_string(struct tfa987x_priv *priv, size_t off)
{
	const char *s = (const char *)priv->fw->data + off;
	size_t max = priv->fw->size - off;

	return strnlen(s, max) == max ? NULL : s;
}

/* the device list whose I2C address is this amplifier's */
static int cnt_find_dev(struct tfa987x_priv *priv)
{
	u16 ndev;
	int i;

	if (priv->fw->size < TFA_CNT_HDR_LEN)
		return -EINVAL;

	ndev = get_unaligned_le16(priv->fw->data + 0x28);

	for (i = 0; i < ndev; i++) {
		size_t off, doff;
		u8 type;
		u8 addr;

		if (cnt_desc(priv, TFA_CNT_HDR_LEN + 4 * i, &off, &type))
			return -EINVAL;
		if (type != DSC_DEVICE)
			continue;
		if (!cnt_has(priv, off, 12))
			return -EINVAL;
		addr = priv->fw->data[off + 2];
		if (addr != priv->addr)
			continue;
		priv->dev_off = off;
		dev_info(priv->dev, "container: device list %d for 0x%02x\n",
			 i, addr);
		return 0;
	}

	return -ENOENT;
}

/* the Nth profile of this amplifier's device list (a dscProfile item) */
static int cnt_profile_off(struct tfa987x_priv *priv, int idx, size_t *poff)
{
	const u8 *buf = priv->fw->data;
	size_t off = priv->dev_off;
	int i, hit = 0;

	u8 len = buf[off];

	for (i = 0; i < len; i++) {
		size_t doff;
		u8 type;

		if (cnt_desc(priv, off + 12 + 4 * i, &doff, &type))
			return -EINVAL;
		if (type != DSC_PROFILE)
			continue;
		if (hit++ == idx) {
			*poff = doff;
			return 0;
		}
	}

	return -ENOENT;
}

static int cnt_nprofiles(struct tfa987x_priv *priv)
{
	const u8 *buf = priv->fw->data;
	size_t off = priv->dev_off;
	int i, n = 0;

	for (i = 0; i < buf[off]; i++) {
		size_t doff;
		u8 type;

		if (cnt_desc(priv, off + 12 + 4 * i, &doff, &type))
			break;
		if (type == DSC_PROFILE)
			n++;
	}

	return n;
}

static const char *cnt_profile_name(struct tfa987x_priv *priv, int idx)
{
	size_t poff, noff;
	u8 type;

	if (cnt_profile_off(priv, idx, &poff) ||
	    cnt_desc(priv, poff + 4, &noff, &type) || type != DSC_STRING)
		return NULL;

	return cnt_string(priv, noff);
}

/* the AUDFS field of a profile, or 0 when the profile does not set one */
static unsigned int cnt_profile_audfs(struct tfa987x_priv *priv, int idx)
{
	size_t poff;
	u8 n, i;

	if (cnt_profile_off(priv, idx, &poff))
		return 0;

	n = priv->fw->data[poff] - 1;
	for (i = 0; i < n; i++) {
		size_t doff;
		u8 type;

		if (cnt_desc(priv, poff + 8 + 4 * i, &doff, &type))
			return 0;
		if (type != DSC_BITFIELD || !cnt_has(priv, doff, 4))
			continue;
		if (get_unaligned_le16(priv->fw->data + doff + 2) == BF_AUDFS)
			return get_unaligned_le16(priv->fw->data + doff);
	}

	return 0;
}

static int tfa987x_cnt_load(struct tfa987x_priv *priv)
{
	const u8 *buf = priv->fw->data;
	size_t len = priv->fw->size;
	u32 size, crc, crc_calc;
	u16 subversion;

	if (len < TFA_CNT_HDR_LEN) {
		dev_err(priv->dev, "container: too short (%zu B)\n", len);
		return -EINVAL;
	}

	if (buf[0] != 'P' || buf[1] != 'M' || buf[2] != '3' || buf[3] != '_') {
		dev_err(priv->dev, "container: not a PM3 container\n");
		return -EINVAL;
	}

	size = get_unaligned_le32(buf + 0x06);
	crc = get_unaligned_le32(buf + TFA_CNT_CRCOFF);
	if (size != len) {
		dev_err(priv->dev, "container: size %u, file %zu\n", size, len);
		return -EINVAL;
	}

	crc_calc = ~crc32_le(~0u, buf + TFA_CNT_CRCOFF + 4,
			     len - TFA_CNT_CRCOFF - 4);
	if (crc_calc != crc) {
		dev_err(priv->dev, "container: CRC 0x%08x, computed 0x%08x\n",
			crc, crc_calc);
		return -EINVAL;
	}

	subversion = get_unaligned_le16(buf + 0x04);
	if ((u8)(subversion >> 8) != '1' && (u8)subversion != '0') {
		dev_err(priv->dev, "container: unsupported subversion\n");
		return -EINVAL;
	}

	dev_info(priv->dev,
		 "container: %.8s %.8s %.8s v%c%c%c, %zu B, CRC ok\n",
		 buf + 0x10, buf + 0x18, buf + 0x20,
		 buf[2], buf[3], buf[4], len);

	return cnt_find_dev(priv);
}

/* --- register and bitfield writers --------------------------------------- */

static int tfa987x_bf_write(struct tfa987x_priv *priv, u16 field, u16 value)
{
	unsigned int reg = BF_REG(field);
	unsigned int mask = ((1u << BF_LEN(field)) - 1) << BF_POS(field);
	unsigned int old, new;
	int ret;

	ret = regmap_read(priv->regmap, reg, &old);
	if (ret)
		return ret;

	new = (old & ~mask) | ((value << BF_POS(field)) & mask);
	if (new == old)
		return 0;

	return regmap_write(priv->regmap, reg, new);
}

static int tfa987x_bf_read(struct tfa987x_priv *priv, u16 field,
			   unsigned int *value)
{
	unsigned int mask = ((1u << BF_LEN(field)) - 1) << BF_POS(field);
	unsigned int reg;
	int ret;

	ret = regmap_read(priv->regmap, BF_REG(field), &reg);
	if (ret)
		return ret;

	*value = (reg & mask) >> BF_POS(field);

	return 0;
}

static int tfa987x_reg_write_masked(struct tfa987x_priv *priv, size_t off)
{
	const u8 *p = priv->fw->data + off;
	u8 addr = p[0];
	u16 value = get_unaligned_le16(p + 1);
	u16 mask = get_unaligned_le16(p + 3);
	unsigned int old;
	int ret;

	ret = regmap_read(priv->regmap, addr, &old);
	if (ret)
		return ret;

	old &= ~mask;
	old |= value & mask;

	return regmap_write(priv->regmap, addr, old);
}

/*
 * Write the register and bitfield items of a descriptor list.  @stop_default
 * ends at the profile's default section, which is the cold-boot half of the
 * list and not part of a profile switch (tfaContWriteRegsProf_v6,
 * tfa_container_v6.c:1140).  A patch, file or profile item ends a device
 * list (tfaContWriteRegsDev_v6_v6, :1108).
 */
static int tfa987x_write_list(struct tfa987x_priv *priv, size_t off, u8 len,
			      bool stop_default)
{
	int i;

	for (i = 0; i < len; i++) {
		size_t doff;
		u8 type;
		int ret;

		ret = cnt_desc(priv, off + 4 * i, &doff, &type);
		if (ret)
			return ret;

		switch (type) {
		case DSC_BITFIELD:
			if (!cnt_has(priv, doff, 4))
				return -EINVAL;
			ret = tfa987x_bf_write(priv,
					       get_unaligned_le16(priv->fw->data + doff + 2),
					       get_unaligned_le16(priv->fw->data + doff));
			break;
		case DSC_REGISTER:
			if (!cnt_has(priv, doff, 5))
				return -EINVAL;
			ret = tfa987x_reg_write_masked(priv, doff);
			break;
		case DSC_DEFAULT:
			if (stop_default)
				return 0;
			continue;
		case DSC_PATCH:
		case DSC_FILE:
		case DSC_PROFILE:
			if (!stop_default)
				return 0;
			continue;
		default:
			continue;
		}

		if (ret)
			return ret;
	}

	return 0;
}

static int tfa987x_write_dev_regs(struct tfa987x_priv *priv)
{
	const u8 *buf = priv->fw->data;
	u8 len = buf[priv->dev_off];

	/* length(1) bus(1) dev(1) func(1) devid(4) name(4), then the items */
	return tfa987x_write_list(priv, priv->dev_off + 12, len, false);
}

static int tfa987x_write_profile(struct tfa987x_priv *priv, int idx)
{
	size_t poff;
	u8 len;

	if (cnt_profile_off(priv, idx, &poff))
		return -ENOENT;

	len = priv->fw->data[poff] - 1;

	return tfa987x_write_list(priv, poff + 8, len, true);
}

/* --- the bring-up sequence ---------------------------------------------- */

static int tfa987x_wait_clks(struct tfa987x_priv *priv)
{
	unsigned int clks;
	int i, ret = -ETIMEDOUT;

	for (i = 0; i < TFA987X_CLK_TRIES; i++) {
		ret = tfa987x_bf_read(priv, BF_CLKS, &clks);
		if (ret)
			return ret;
		if (clks)
			return 0;
		msleep(TFA987X_CLK_POLL_MS);
	}

	dev_err(priv->dev, "no stable clock after %d ms\n",
		TFA987X_CLK_TRIES * TFA987X_CLK_POLL_MS);
	return ret;
}

static void tfa987x_show_status(struct tfa987x_priv *priv, const char *when)
{
	unsigned int clks = 0, plls = 0, amps = 0, arefs = 0, sws = 0;
	unsigned int st1 = 0, st2 = 0;
	int ret;

	ret = regmap_read(priv->regmap, TFA987X_REG_STATUS1, &st1);
	if (!ret)
		ret = regmap_read(priv->regmap, TFA987X_REG_STATUS2, &st2);
	if (ret) {
		dev_err(priv->dev, "status read failed: %d\n", ret);
		return;
	}

	tfa987x_bf_read(priv, BF_CLKS, &clks);
	tfa987x_bf_read(priv, BF_PLLS, &plls);
	tfa987x_bf_read(priv, BF_AMPS, &amps);
	tfa987x_bf_read(priv, BF_AREFS, &arefs);
	tfa987x_bf_read(priv, BF_SWS, &sws);

	dev_info(priv->dev,
		 "%s: 0x10=0x%04x 0x11=0x%04x CLKS=%u PLLS=%u AREFS=%u AMPS=%u SWS=%u\n",
		 when, st1, st2, clks, plls, arefs, amps, sws);
	dev_info(priv->dev,
		 "%s: fault bits OTDS/OCDS/UVDS/MANALARM/NOCLK/TDMERR/DCIL/TDMLUTER = %u%u%u%u%u%u%u%u\n",
		 when,
		 !!(st1 & BIT(2)), !!(st1 & BIT(3)), !!(st1 & BIT(4)),
		 !!(st1 & BIT(5)), !!(st1 & BIT(8)), !!(st1 & BIT(10)),
		 !!(st2 & BIT(0)), !!(st2 & BIT(8)));
}

/*
 * The vendor's cold start for a probus part (tfa_dsp_v6.c:3132-3295,
 * 4220-4330): reset the I2C register file, write the device list's interface
 * and protection settings, write the profile, leave powerdown and wait for
 * the clock, then set MANSCONF and the amplifier.  The DSP half of the
 * sequence is the no-op the phone's own build compiles; there is no DSP.
 */
static int tfa987x_configure(struct tfa987x_priv *priv)
{
	int ret;

	ret = tfa987x_bf_write(priv, BF_I2CR, 1);
	if (ret)
		return ret;
	usleep_range(1000, 2000);

	ret = tfa987x_write_dev_regs(priv);
	if (ret) {
		dev_err(priv->dev, "device list write failed: %d\n", ret);
		return ret;
	}

	ret = tfa987x_write_profile(priv, priv->profile);
	if (ret) {
		dev_err(priv->dev, "profile %d write failed: %d\n",
			priv->profile, ret);
		return ret;
	}

	ret = tfa987x_bf_write(priv, BF_PWDN, 0);
	if (ret)
		return ret;

	ret = tfa987x_wait_clks(priv);
	if (ret)
		return ret;

	priv->configured = true;
	dev_info(priv->dev, "configured, profile %d (%s) at %u Hz\n",
		 priv->profile, cnt_profile_name(priv, priv->profile),
		 priv->rate);

	return 0;
}

static int tfa987x_unmute(struct tfa987x_priv *priv)
{
	unsigned int plls;
	int ret;

	ret = tfa987x_bf_write(priv, BF_PWDN, 0);
	if (ret)
		return ret;

	ret = tfa987x_bf_write(priv, BF_MANSCONF, 1);
	if (ret)
		return ret;

	ret = tfa987x_wait_clks(priv);
	if (ret)
		return ret;

	ret = tfa987x_bf_read(priv, BF_PLLS, &plls);
	if (ret)
		return ret;
	if (!plls)
		dev_warn(priv->dev, "PLL not locked at unmute\n");

	ret = tfa987x_bf_write(priv, BF_AMPE, 1);
	if (ret)
		return ret;

	priv->running = true;
	tfa987x_show_status(priv, "unmute");

	return 0;
}

static int tfa987x_mute(struct tfa987x_priv *priv)
{
	int ret;

	/* Read the amplifier's own state first: this is the last moment the
	 * device is still running, so it is the record of what the stream did.
	 */
	if (priv->running)
		tfa987x_show_status(priv, "running");

	ret = tfa987x_bf_write(priv, BF_AMPE, 0);
	if (ret)
		return ret;

	priv->running = false;

	return 0;
}

/* --- ASoC ---------------------------------------------------------------- */

static int tfa987x_select_profile(struct tfa987x_priv *priv, unsigned int rate)
{
	unsigned int audfs = rate_to_audfs(rate);
	int i;
	const char *name;

	if ((int)audfs < 0)
		return -EINVAL;

	/* Prefer the loudspeaker profile: "speaker" on 0x34, "speakerR" on 0x35. */
	for (i = 0; (name = cnt_profile_name(priv, i)); i++) {
		if (strncasecmp(name, "speaker", 7))
			continue;
		if (cnt_profile_audfs(priv, i) == audfs)
			return i;
	}

	/* Then any profile that carries this rate. */
	for (i = 0; (name = cnt_profile_name(priv, i)); i++) {
		if (cnt_profile_audfs(priv, i) == audfs) {
			dev_warn(priv->dev, "no speaker profile, using '%s'\n",
				 name);
			return i;
		}
	}

	return -EINVAL;
}

static int tfa987x_dai_startup(struct snd_pcm_substream *substream,
			       struct snd_soc_dai *dai)
{
	struct tfa987x_priv *priv = snd_soc_component_get_drvdata(dai->component);

	if (!priv->fw) {
		dev_err(dai->dev, "no container: amplifier stays off\n");
		return -ENODEV;
	}

	dev_info(dai->dev, "startup, %d profiles\n", cnt_nprofiles(priv));

	return 0;
}

static int tfa987x_dai_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params,
				 struct snd_soc_dai *dai)
{
	struct tfa987x_priv *priv = snd_soc_component_get_drvdata(dai->component);
	unsigned int rate = params_rate(params);
	int ret, prof;

	dev_info(dai->dev, "hw_params: %u Hz, %u ch, %u-bit\n", rate,
		 params_channels(params),
		 snd_pcm_format_width(params_format(params)));

	prof = tfa987x_select_profile(priv, rate);
	if (prof < 0) {
		dev_err(dai->dev, "no profile for %u Hz\n", rate);
		return prof;
	}

	mutex_lock(&priv->lock);
	priv->profile = prof;
	priv->rate = rate;
	ret = tfa987x_configure(priv);
	mutex_unlock(&priv->lock);

	return ret;
}

static int tfa987x_dai_trigger(struct snd_pcm_substream *substream, int cmd,
			       struct snd_soc_dai *dai)
{
	struct tfa987x_priv *priv = snd_soc_component_get_drvdata(dai->component);
	int ret = 0;

	mutex_lock(&priv->lock);

	if (!priv->configured) {
		mutex_unlock(&priv->lock);
		return 0;
	}

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		ret = tfa987x_unmute(priv);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		ret = tfa987x_mute(priv);
		break;
	}

	mutex_unlock(&priv->lock);

	return ret;
}

static int tfa987x_dai_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	/*
	 * The board's link declares no dai-format, so this is normally not
	 * called at all; when it is, accept what the container needs: the AP
	 * is the I2S provider and this amplifier is the consumer (the DT's
	 * reset-gpio/pinctrl have the MI2S as master).
	 */
	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
	case SND_SOC_DAIFMT_DSP_A:
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct snd_soc_dai_ops tfa987x_dai_ops = {
	.startup = tfa987x_dai_startup,
	.hw_params = tfa987x_dai_hw_params,
	.trigger = tfa987x_dai_trigger,
	.set_fmt = tfa987x_dai_set_fmt,
};

static struct snd_soc_dai_driver tfa987x_dai_driver[] = {
	{
		.name = "tfa987x-aif",
		.playback = {
			.stream_name = "Speaker Playback",
			.formats = SNDRV_PCM_FMTBIT_S32_LE,
			.rates = SNDRV_PCM_RATE_48000,
			.rate_min = 48000,
			.rate_max = 48000,
			.channels_min = 2,
			.channels_max = 2,
		},
		.ops = &tfa987x_dai_ops,
	},
};

static const struct snd_soc_component_driver tfa987x_component_driver = {
	.name = "tfa987x",
};

/* --- probe --------------------------------------------------------------- */

static int tfa987x_reset(struct device *dev, struct gpio_desc *reset)
{
	gpiod_set_value_cansleep(reset, 1);
	usleep_range(TFA987X_RESET_PULSE_US, TFA987X_RESET_PULSE_US + 1000);
	gpiod_set_value_cansleep(reset, 0);
	usleep_range(TFA987X_RESET_PULSE_US, TFA987X_RESET_PULSE_US + 1000);

	return 0;
}

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
	priv->addr = client->addr;
	mutex_init(&priv->lock);
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
		dev_info(dev, "TFA9873 revision 0x%04x at 0x%02x\n", rev,
			 priv->addr);
		break;
	case TFA9874_ID:
		dev_info(dev, "TFA9874 revision 0x%04x at 0x%02x\n", rev,
			 priv->addr);
		break;
	default:
		dev_err(dev, "unexpected revision 0x%04x\n", rev);
		return -ENODEV;
	}

	priv->profile = -1;

	/* The container is shared by both amplifiers; each keeps its own copy. */
	ret = request_firmware(&priv->fw, "tfa98xx.cnt", dev);
	if (ret) {
		dev_err(dev, "no /lib/firmware/tfa98xx.cnt: %d\n", ret);
		priv->fw = NULL;
	} else if (tfa987x_cnt_load(priv)) {
		dev_err(dev, "container rejected: amplifier stays off\n");
		release_firmware(priv->fw);
		priv->fw = NULL;
	}

	/* Leave the amplifier muted until a stream configures it. */
	if (priv->fw) {
		mutex_lock(&priv->lock);
		tfa987x_mute(priv);
		mutex_unlock(&priv->lock);
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

MODULE_DESCRIPTION("NXP TFA987x speaker amplifier, container driven");
MODULE_LICENSE("GPL");
