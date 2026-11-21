// SPDX-License-Identifier: GPL-2.0-only
/*
 * OnePlus AA553-P-7-A0001 AMOLED DSI panel (OnePlus Nord 4, Avalon).
 *
 * The panel is a 1240x2772 command-mode AMOLED with two DSC encoders. Its
 * initialisation sequence, DSC parameters, timings and rail order are the
 * downstream OnePlus device tree's
 * (android_kernel_modules_and_devicetree_oneplus_sm7675,
 * oneplus/sm7675_b_16.0.0_nord_4, commit 150f7f8f3354dc680da90e83788bd25d9b983739):
 * vendor/qcom/proprietary/display-devicetree/oplus/panel/dsi-panel-AA553-P-7-A0001-dsc-cmd.dtsi
 * and .../oplus/audi-23803-display-cliffs-overlay.dtsi.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>

#include <drm/display/drm_dsc.h>
#include <drm/display/drm_dsc_helper.h>

#include <drm/drm_crtc.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

#include <video/mipi_display.h>

struct aa553_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct drm_dsc_config dsc;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data *supplies;
	struct drm_connector *connector;
	unsigned int refresh_hz;	/* last rate the DDIC was told to run */
	bool prepared;
};

static const struct regulator_bulk_data aa553_supplies[] = {
	{ .supply = "vddio" },	/* 1.8 V, gated before vci */
	{ .supply = "vci" },	/* 3.0 V */
};

static inline struct aa553_panel *to_aa553_panel(struct drm_panel *panel)
{
	return container_of(panel, struct aa553_panel, panel);
}

#define AA553_DCS_SWITCH_PAGE	0xf0

#define aa553_switch_page(dsi_ctx, page) \
	mipi_dsi_dcs_write_seq_multi((dsi_ctx), AA553_DCS_SWITCH_PAGE, \
				     0x55, 0xaa, 0x52, 0x08, (page))

/*
 * Reset is active low: gpiod value 0 drives the line high, 1 drives it low.
 * The downstream qcom,mdss-dsi-reset-sequence is <1 2>, <0 5>, <1 25>, i.e.
 * high 2 ms, low 5 ms, high 25 ms (dsi-panel-AA553-P-7-A0001-dsc-cmd.dtsi:25).
 */
static void aa553_reset(struct aa553_panel *ctx)
{
	/*
	 * The vendor's qcom,mdss-dsi-reset-sequence is
	 * <1 2>, <0 5>, <1 25> (dsi-panel-AA553-P-7-A0001-dsc-cmd.dtsi:25)
	 * on an ACTIVE_HIGH gpio: release 2 ms, assert 5 ms, release 25 ms.
	 * Our DT declares tlmm 79 GPIO_ACTIVE_LOW, so the same physical order
	 * is 0, 1, 0.  Every sleep doubled; writing the vendor's numeric
	 * levels here inverted it and held the DDIC in reset (measured:
	 * on ret=-110, reads -110/-22).
	 */
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(4000, 5000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(50000, 51000);
}

/*
 * The 120 Hz timing's initialisation sequence, line for line from
 * qcom,mdss-dsi-on-command (dsi-panel-AA553-P-7-A0001-dsc-cmd.dtsi:90-200).
 * The downstream table's 0x91 short write is the panel's own DSC
 * configuration; the VESA PPS itself is sent by the driver in prepare().
 */
/*
 * The VESA PPS.  The standard DCS 0x07 compression-mode write is not sent:
 * the vendor's table never contains it and the DDIC's own status byte (0x03)
 * reads its error value when it is sent (F0160 §4, F0168).
 */
static void aa553_send_pps(struct aa553_panel *ctx,
			   struct mipi_dsi_multi_context *dsi_ctx)
{
	struct drm_dsc_picture_parameter_set pps;

	drm_dsc_pps_payload_pack(&pps, &ctx->dsc);
	mipi_dsi_picture_parameter_set_multi(dsi_ctx, &pps);

	/* vendor PPS-to-display-on 28 ms, doubled */
	mipi_dsi_msleep(dsi_ctx, 56);
}

static int aa553_on(struct aa553_panel *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = dsi };

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	/* DVDD strong */
	aa553_switch_page(&dsi_ctx, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x05);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc5, 0x15, 0x15, 0x15, 0xdd);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x81);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x19);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfb, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x05);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfe, 0x3c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x80);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x1a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf4, 0x55);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x83);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x12);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfe, 0x41);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x5f, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x26, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x26, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x17, 0x10);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x2a, 0x00, 0x00, 0x04, 0xd7);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x2b, 0x00, 0x00, 0x0a, 0xd3);
	/* 120 Hz; 0x01 = 120, 0x02 = 90, 0x03 = 60 */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x2f, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x35, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x44, 0x00, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x53, 0x20);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x90, 0x03, 0x03);

	/* DSC setting: 10 bit, 3.75 bit per pixel, DSC 1.2 */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x91,
				     0xab, 0xa8, 0x00, 0x0c, 0xc2, 0x00, 0x02, 0x37, 0x01, 0x33,
				     0x00, 0x08, 0x08, 0xbb, 0x07, 0x5f, 0x10, 0xf0);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x81);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x24);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfb,
				     0x00, 0x03, 0x04, 0x55, 0x77, 0x77, 0x77, 0x99, 0x9d, 0x00,
				     0x00, 0x06, 0x88, 0x9a, 0xbb, 0xbc, 0xde, 0xef, 0xf0, 0x11);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x0d);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfb, 0x80);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x81);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x0e);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf5, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x80);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x0f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfc, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x09);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfc, 0xfc, 0xf0);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x80);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x0a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf6, 0x70, 0x70, 0x70);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x0e);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf6, 0x60);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x80);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x11);
	/* OSC2 = 138.6 MHz */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf8, 0x01, 0x7c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x2d);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf8, 0x00, 0xfc);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x1f, 0x06);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x81);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x1e);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfb, 0x0f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x83);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x12);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfe, 0x41);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x80);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x19);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf2, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xaa, 0x55, 0xa5, 0x81);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf9, 0x04);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x5a, 0x00);

	/* OSC tracking at 556 MHz */
	aa553_switch_page(&dsi_ctx, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc3,
				     0xdd, 0x06, 0x22, 0x11, 0xfc, 0x00, 0x06, 0x20, 0x11, 0xff,
				     0x00, 0x03, 0xf2, 0x12, 0x05, 0xfc, 0x18, 0x03, 0xf2, 0x12,
				     0x05, 0xfc, 0x18, 0x03, 0xf2, 0x12, 0x05, 0xfc, 0x18, 0x03,
				     0xf2, 0x12, 0x05, 0xfc, 0x18, 0x03, 0xf2, 0x12, 0x05, 0xfc,
				     0x18);

	/* LHBM icon position */
	aa553_switch_page(&dsi_ctx, 0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x88,
				     0x01, 0x02, 0x6c, 0x09, 0xd4, 0x00, 0x00, 0x00, 0x00);

	/* LHBM em duty */
	aa553_switch_page(&dsi_ctx, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x31);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xdf, 0x20);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x38);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xdf, 0x0a, 0xec);

	/* Increase the number of VFP rows */
	aa553_switch_page(&dsi_ctx, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xba,
				     0x00, 0x58, 0x00, 0x20, 0x00, 0x20, 0x10, 0x00, 0x58, 0x00,
				     0x20, 0x03, 0xd4, 0x10, 0x00, 0x58, 0x00, 0x20, 0x0b, 0x38,
				     0x10);
	/*
	 * The vendor's table selects page 0 again right before the decoder
	 * tables (dsi-panel-AA553-P-7-A0001-dsc-cmd.dtsi:76); without it the
	 * first 0xb4 lands in the page the 0xba write left.
	 */
	aa553_switch_page(&dsi_ctx, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb4,
				     0x00, 0x00, 0x00, 0xc0, 0x00, 0xdc, 0x00, 0xdc, 0x01, 0x10,
				     0x01, 0x10, 0x01, 0xa0, 0x01, 0xa0, 0x02, 0xb8, 0x02, 0xb8,
				     0x04, 0xe8, 0x04, 0xe8, 0x08, 0x30, 0x08, 0x30, 0x0a, 0xec,
				     0x0a, 0xec, 0x0a, 0xec, 0x0a, 0xec, 0x0a, 0xec, 0x0a, 0xec,
				     0x0a, 0xec, 0x0a, 0xec, 0x0a, 0xec);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x2e);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb4,
				     0x00, 0x00, 0x01, 0x20, 0x01, 0x44, 0x01, 0x44, 0x01, 0x90,
				     0x01, 0x90, 0x02, 0x44, 0x02, 0x44, 0x03, 0xb0, 0x03, 0xb0,
				     0x06, 0x8c, 0x06, 0x8c, 0x0a, 0xd0, 0x0a, 0xd0, 0x0e, 0x94,
				     0x0e, 0x94, 0x0e, 0x94, 0x0e, 0x94, 0x0e, 0x94, 0x0e, 0x94,
				     0x0e, 0x94, 0x0e, 0x94, 0x0e, 0x94);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x5c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb4,
				     0x00, 0x00, 0x01, 0x80, 0x01, 0xb8, 0x01, 0xb8, 0x02, 0x28,
				     0x02, 0x28, 0x03, 0x40, 0x03, 0x40, 0x05, 0x70, 0x05, 0x70,
				     0x09, 0xd4, 0x09, 0xd4, 0x10, 0x64, 0x10, 0x64, 0x15, 0xe0,
				     0x15, 0xe0, 0x15, 0xe0, 0x15, 0xe0, 0x15, 0xe0, 0x15, 0xe0,
				     0x15, 0xe0, 0x15, 0xe0, 0x15, 0xe0);

	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	/* vendor 0x78 = 120 ms after sleep-out, doubled */
	mipi_dsi_msleep(&dsi_ctx, 240);
	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);

	return dsi_ctx.accum_err;
}

static int aa553_off(struct aa553_panel *ctx)
{
	struct mipi_dsi_device *dsi = ctx->dsi;
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = dsi };

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	/* qcom,mdss-dsi-off-command, dsi-panel-AA553-P-7-A0001-dsc-cmd.dtsi:205-209 */
	aa553_switch_page(&dsi_ctx, 0x00);
	mipi_dsi_msleep(&dsi_ctx, 70);
	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 10);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);

	return dsi_ctx.accum_err;
}

/*
 * qcom,mdss-dsi-timing-switch-command for the three timing nodes
 * (dsi-panel-AA553-P-7-A0001-dsc-cmd.dtsi:80-89, 331-340, 455-464).
 * The six entries of each array are 0x2F <rate>, a page-0 switch, and the
 * page-0 em-duty pair; the byte the vendor generator put in the ack field
 * (0x60, 0x20) is not a wait: the wait field (byte 4) is 0 in all six
 * (Documentation/devicetree/bindings/fb/mdss-dsi-panel.txt, byte 4 "wait
 * number of specified ms").  The on-command's own 120 Hz writes are
 * 0x2F/0x6F/0xDF too, so a switch is the same three registers.
 */
struct aa553_refresh {
	unsigned int vrefresh;
	u8 frame_rate;		/* DCS 0x2F: 01 = 120, 02 = 90, 03 = 60 Hz */
	u8 em_duty_31;
	u8 em_duty_38[2];
};

static const struct aa553_refresh aa553_refresh[] = {
	{ 120, 0x01, 0x20, { 0x0a, 0xec } },
	{  90, 0x02, 0x21, { 0x0e, 0x94 } },
	{  60, 0x03, 0x22, { 0x15, 0xe0 } },
};

static const struct aa553_refresh *aa553_rate(unsigned int vrefresh)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(aa553_refresh); i++)
		if (aa553_refresh[i].vrefresh == vrefresh)
			return &aa553_refresh[i];

	return NULL;
}

/*
 * The DDIC's timing switch: 2F selects the frame rate, the page-0 em-duty
 * pair the emission timing that goes with it.  Sent in high speed mode over
 * the live link; the DDIC takes the new timing at its next frame, so the
 * TE line never stops and nothing on the host side is reprogrammed.
 */
static int aa553_send_timing_switch(struct aa553_panel *ctx,
				    const struct aa553_refresh *rate)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	mipi_dsi_dcs_write_var_seq_multi(&dsi_ctx, 0x2f, rate->frame_rate);
	aa553_switch_page(&dsi_ctx, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x31);
	mipi_dsi_dcs_write_var_seq_multi(&dsi_ctx, 0xdf, rate->em_duty_31);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x6f, 0x38);
	mipi_dsi_dcs_write_var_seq_multi(&dsi_ctx, 0xdf, rate->em_duty_38[0],
					 rate->em_duty_38[1]);

	return dsi_ctx.accum_err;
}

/*
 * The rate KMS selected is not handed to a drm_panel.  A mode change makes
 * the atomic helpers disable and re-enable the whole bridge chain, so
 * .prepare() runs again, and by then drm_atomic_helper_swap_state() has put
 * the new mode into the connector's state.  Read the adjusted mode from
 * there and send the timing switch after the init table, which is the 120 Hz
 * table.
 */
static int aa553_apply_refresh_rate(struct aa553_panel *ctx)
{
	const struct drm_display_mode *mode;
	const struct aa553_refresh *rate;
	unsigned int vrefresh = 120;

	if (ctx->connector && ctx->connector->state && ctx->connector->state->crtc) {
		mode = &ctx->connector->state->crtc->state->adjusted_mode;
		if (mode->htotal && mode->vtotal)
			vrefresh = drm_mode_vrefresh(mode);
	}

	rate = aa553_rate(vrefresh);
	if (!rate) {
		dev_warn(ctx->panel.dev,
			 "%u Hz is not one of the panel's modes; leaving 120 Hz\n",
			 vrefresh);
		return 0;
	}

	if (rate->vrefresh == 120) {
		ctx->refresh_hz = 120;	/* the init table programmed it */
		return 0;
	}

	if (aa553_send_timing_switch(ctx, rate) < 0)
		return -EIO;

	ctx->refresh_hz = rate->vrefresh;

	return 0;
}

/*
 * A same-resolution mode change is not a modeset: msm clears mode_changed
 * for it and calls this from the atomic commit tail, so the CRTC, the DSI
 * link and the panel's init table all stay up and only the DDIC's frame
 * timing changes.  Measured: no TE gap and no DSI off/on across the switch
 * (F0337).
 */
static int aa553_set_refresh_rate(struct drm_panel *panel, unsigned int vrefresh)
{
	struct aa553_panel *ctx = to_aa553_panel(panel);
	const struct aa553_refresh *rate = aa553_rate(vrefresh);
	unsigned int old;

	if (!rate) {
		dev_warn(ctx->panel.dev, "%u Hz is not one of the panel's modes\n",
			 vrefresh);
		return -EINVAL;
	}

	/* .prepare() keys the DDIC on the mode it runs for; nothing to do yet */
	if (!ctx->prepared || rate->vrefresh == ctx->refresh_hz)
		return 0;

	old = ctx->refresh_hz;

	if (aa553_send_timing_switch(ctx, rate) < 0) {
		dev_err(ctx->panel.dev, "timing switch %u -> %u Hz failed\n",
			old, rate->vrefresh);
		return -EIO;
	}

	ctx->refresh_hz = rate->vrefresh;
	dev_info(ctx->panel.dev, "refresh %u -> %u Hz, no modeset\n",
		 old, rate->vrefresh);

	return 0;
}

/*
 * Rail order and delays are the downstream supply entries
 * (audi-23803-display-cliffs-overlay.dtsi:21-43): vddio, then its 2 ms
 * post-on and vci's 1 ms pre-on, then vci, then 15 ms, then the panel's
 * qcom,mdss-dsi-init-delay-us of 1 ms before the first command. teardown is
 * the reverse: vci off 1 ms after the reset is asserted, vddio off 30 ms
 * after that.
 */
static int aa553_prepare(struct drm_panel *panel)
{
	struct aa553_panel *ctx = to_aa553_panel(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };
	int ret;

	/*
	 * Root cause (F0239, F0244): a first mode set after a warm boot lit
	 * only 1/6 because the bootloader leaves both rails on and the panel
	 * running, so a plain regulator_enable() is a no-op and the DDIC never
	 * sees a real power cycle; a hot reset of a running panel is the race.
	 * Force the vendor's power-off first: reset asserted, vci off, vddio
	 * off, 200 ms, and give every vendor wait double its value.
	 */
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	regulator_force_disable(ctx->supplies[1].consumer);
	regulator_force_disable(ctx->supplies[0].consumer);
	usleep_range(200000, 210000);

	ret = regulator_enable(ctx->supplies[0].consumer);
	if (ret < 0)
		return ret;

	/* vendor 2 ms post-on, doubled */
	usleep_range(4000, 5000);

	/*
	 * The vci entry also asks for a 1 ms pre-on sleep
	 * (audi-23803-display-cliffs-overlay.dtsi:39), so the rail comes up
	 * 3 ms after vddio, not 2 ms.
	 */
	/* vendor 1 ms pre-on, doubled */
	usleep_range(2000, 3000);

	ret = regulator_enable(ctx->supplies[1].consumer);
	if (ret < 0)
		goto err_disable_vddio;

	/* vendor 15 ms after vci, doubled */
	usleep_range(30000, 31000);

	aa553_reset(ctx);
	/* vendor init-delay-us 1000, doubled */
	usleep_range(2000, 3000);

	ret = aa553_on(ctx);
	if (ret < 0)
		goto err_disable;

	/* the vendor sends the PPS from the frame update, after display-on */
	aa553_send_pps(ctx, &dsi_ctx);

	if (dsi_ctx.accum_err < 0) {
		ret = dsi_ctx.accum_err;
		goto err_disable;
	}

	/*
	 * The init table is sent in low-power mode
	 * (qcom,mdss-dsi-on-command-state = "dsi_lp_mode") but everything
	 * after it, the brightness writes included, is high-speed
	 * (qcom,bl-dsc-cmd-state = "dsi_hs_mode").  The timing switch is
	 * high-speed too (qcom,mdss-dsi-timing-switch-command-state).
	 */
	ctx->dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = aa553_apply_refresh_rate(ctx);
	if (ret < 0)
		goto err_disable;

	ctx->prepared = true;

	return 0;

err_disable:
	regulator_disable(ctx->supplies[1].consumer);
err_disable_vddio:
	regulator_disable(ctx->supplies[0].consumer);
	return ret;
}

static int aa553_unprepare(struct drm_panel *panel)
{
	struct aa553_panel *ctx = to_aa553_panel(panel);

	aa553_off(ctx);
	ctx->prepared = false;

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);

	usleep_range(1000, 2000);
	regulator_disable(ctx->supplies[1].consumer);
	usleep_range(2000, 3000);
	usleep_range(30000, 31000);
	regulator_disable(ctx->supplies[0].consumer);

	return 0;
}

/*
 * The three timing nodes of the vendor panel DT, same resolution and DSC
 * geometry, per-mode porches (dsi-panel-AA553-P-7-A0001-dsc-cmd.dtsi
 * timing@ nodes; survey in results/display-panel-modes/survey.txt §1):
 *
 *   rate  hfp  hsw  hbp  vfp  vsw  vbp   htotal vtotal
 *   120    64    8   50    2    2    8    1362   2784
 *    90    64    8   80   10    2   16    1392   2800
 *    60   100    8  120   20    2   30    1468   2824
 *
 * The 60 Hz node also has qcom,mdss-dsi-h-sync-skew = <1>; DRM has no field
 * for it and the vendor's own frame pixel rate ignores it.
 */
static const struct drm_display_mode aa553_modes[] = {
	{
		/* (1240 + 64 + 8 + 50) * (2772 + 2 + 2 + 8) * 120 / 1000 */
		.clock = (1240 + 64 + 8 + 50) * (2772 + 2 + 2 + 8) * 120 / 1000,
		.hdisplay = 1240,
		.hsync_start = 1240 + 64,
		.hsync_end = 1240 + 64 + 8,
		.htotal = 1240 + 64 + 8 + 50,
		.vdisplay = 2772,
		.vsync_start = 2772 + 2,
		.vsync_end = 2772 + 2 + 2,
		.vtotal = 2772 + 2 + 2 + 8,
		.width_mm = 70,
		.height_mm = 156,
		.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
	},
	{
		/* (1240 + 64 + 8 + 80) * (2772 + 10 + 2 + 16) * 90 / 1000 */
		.clock = (1240 + 64 + 8 + 80) * (2772 + 10 + 2 + 16) * 90 / 1000,
		.hdisplay = 1240,
		.hsync_start = 1240 + 64,
		.hsync_end = 1240 + 64 + 8,
		.htotal = 1240 + 64 + 8 + 80,
		.vdisplay = 2772,
		.vsync_start = 2772 + 10,
		.vsync_end = 2772 + 10 + 2,
		.vtotal = 2772 + 10 + 2 + 16,
		.width_mm = 70,
		.height_mm = 156,
		.type = DRM_MODE_TYPE_DRIVER,
	},
	{
		/* (1240 + 100 + 8 + 120) * (2772 + 20 + 2 + 30) * 60 / 1000 */
		.clock = (1240 + 100 + 8 + 120) * (2772 + 20 + 2 + 30) * 60 / 1000,
		.hdisplay = 1240,
		.hsync_start = 1240 + 100,
		.hsync_end = 1240 + 100 + 8,
		.htotal = 1240 + 100 + 8 + 120,
		.vdisplay = 2772,
		.vsync_start = 2772 + 20,
		.vsync_end = 2772 + 20 + 2,
		.vtotal = 2772 + 20 + 2 + 30,
		.width_mm = 70,
		.height_mm = 156,
		.type = DRM_MODE_TYPE_DRIVER,
	},
};

static int aa553_get_modes(struct drm_panel *panel, struct drm_connector *connector)
{
	struct aa553_panel *ctx = to_aa553_panel(panel);
	unsigned int i;
	int count = 0;

	connector->display_info.width_mm = aa553_modes[0].width_mm;
	connector->display_info.height_mm = aa553_modes[0].height_mm;

	for (i = 0; i < ARRAY_SIZE(aa553_modes); i++) {
		struct drm_display_mode *mode;

		mode = drm_mode_duplicate(connector->dev, &aa553_modes[i]);
		if (!mode) {
			if (!count)
				return -ENOMEM;
			break;
		}
		drm_mode_set_name(mode);
		drm_mode_probed_add(connector, mode);
		count++;
	}

	/* .prepare() keys the DDIC's frame rate on this connector's mode. */
	ctx->connector = connector;

	return count;
}

/*
 * A panel driver is what turns the backlight on: drm_panel_enable() does not.
 * Without this the level is written only when userspace writes the sysfs file,
 * which at boot happens while the panel is unprepared and is skipped, so the
 * DDIC keeps whatever brightness its own reset left.
 */
static int aa553_enable(struct drm_panel *panel)
{
	struct aa553_panel *ctx = to_aa553_panel(panel);

	return backlight_enable(ctx->panel.backlight);
}

static int aa553_disable(struct drm_panel *panel)
{
	struct aa553_panel *ctx = to_aa553_panel(panel);

	backlight_disable(ctx->panel.backlight);

	return 0;
}

static const struct drm_panel_funcs aa553_panel_funcs = {
	.prepare = aa553_prepare,
	.enable = aa553_enable,
	.disable = aa553_disable,
	.unprepare = aa553_unprepare,
	.get_modes = aa553_get_modes,
	.set_refresh_rate = aa553_set_refresh_rate,
};

/*
 * The downstream panel is bl_ctrl_dcs with qcom,mdss-dsi-bl-inverted-dbv and
 * a 0..3999 12-bit range (dsi-panel-AA553-P-7-A0001-dsc-cmd.dtsi:866-869).
 *
 * "inverted dbv" is the DDIC's byte order, not an inverted scale: the vendor
 * swaps the 16-bit level and then sends it with the little-endian DCS helper
 * (oplus/oplus_display_panel_feature.c:620-651,
 * mipi_dsi_dcs_set_display_brightness()), so the panel receives the level
 * big-endian - which is what the _large() helper sends.  A complement
 * (max - level) would put the top of the scale at DBV 0.
 *
 * The raw DBV range is 0..3999, but the panel's normal band ends at 3238
 * (oplus,dsi-bl-normal-max-level, dsi-panel-AA553-P-7-A0001-dsc-cmd.dtsi and
 * ...-common.dtsi).  Above 3238 the stock stack does not send a larger DBV:
 * it sends the HBM command sets (hbm-on/hbm-max and the B6 tune), whose
 * mapping is not in these files.  Keep the raw range and clamp the top here,
 * so userspace's 0..3999 scale cannot walk into a level the panel would show
 * differently; HBM above 3238 needs its own property
 * (docs/plans/panel-modes.md).
 */
#define AA553_BL_NORMAL_MAX	3238

static int aa553_bl_update_status(struct backlight_device *bl)
{
	struct aa553_panel *ctx = bl_get_data(bl);
	int level = backlight_get_brightness(bl);

	/*
	 * Outside prepare()/unprepare() the rails are down and the panel does
	 * not answer: a brightness write would sit out the DSI timeout and
	 * fail. systemd-backlight does exactly that at boot and at shutdown,
	 * so skip it; aa553_enable() applies the level once the panel is up.
	 */
	if (!ctx->prepared)
		return 0;

	return mipi_dsi_dcs_set_display_brightness_large(ctx->dsi,
			min(level, AA553_BL_NORMAL_MAX));
}

static const struct backlight_ops aa553_bl_ops = {
	.update_status = aa553_bl_update_status,
};

static struct backlight_device *aa553_create_backlight(struct aa553_panel *ctx)
{
	struct device *dev = ctx->panel.dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 1638,
		.max_brightness = 3999,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, ctx,
					      &aa553_bl_ops, &props);
}

static int aa553_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct aa553_panel *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct aa553_panel, panel,
				   &aa553_panel_funcs, DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ret = devm_regulator_bulk_get_const(dev, ARRAY_SIZE(aa553_supplies),
					    aa553_supplies, &ctx->supplies);
	if (ret < 0)
		return ret;

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB101010;
	dsi->mode_flags = MIPI_DSI_MODE_NO_EOT_PACKET | MIPI_DSI_CLOCK_NON_CONTINUOUS;

	ctx->panel.prepare_prev_first = true;

	ctx->panel.backlight = aa553_create_backlight(ctx);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
				     "Failed to create backlight\n");

	drm_panel_add(&ctx->panel);

	/*
	 * DSC 1.2, two 620x12 slices for the 1240 pixel line, 10 bit per
	 * component and 8 bit per pixel (dsi-panel-AA553-P-7-A0001-dsc-cmd.dtsi:54,
	 * 289-297).
	 */
	dsi->dsc = &ctx->dsc;
	ctx->dsc.dsc_version_major = 1;
	ctx->dsc.dsc_version_minor = 2;
	ctx->dsc.slice_height = 12;
	ctx->dsc.slice_width = 620;
	ctx->dsc.slice_count = 2;
	ctx->dsc.bits_per_component = 10;
	ctx->dsc.bits_per_pixel = 8 << 4;
	ctx->dsc.block_pred_enable = true;
	/*
	 * qcom,mdss-dsc-slice-per-pkt = <2>
	 * (dsi-panel-AA553-P-7-A0001-dsc-cmd.dtsi:294): both 620x12 slices of a
	 * line travel in one DSI packet, so one packet per line and 1240 bytes
	 * per packet, not two packets of 620.
	 */
	ctx->dsc.dsc_slice_per_pkt = 2;

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "Failed to attach to DSI host\n");
	}

	return 0;
}

static void aa553_remove(struct mipi_dsi_device *dsi)
{
	struct aa553_panel *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	/*
	 * The panel goes first: detaching the DSI device while the DRM panel
	 * and its bridge still exist leaves the bridge pointing at a device
	 * whose host is being torn down (mipi_dsi_detach then fails with
	 * -EINVAL), and the teardown after that can hang.  Mainline panel
	 * drivers remove the panel, then detach.
	 */
	drm_panel_remove(&ctx->panel);

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);
}

static const struct of_device_id aa553_of_match[] = {
	{ .compatible = "oneplus,aa553-p-7-a0001" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, aa553_of_match);

static struct mipi_dsi_driver aa553_driver = {
	.probe = aa553_probe,
	.remove = aa553_remove,
	.driver = {
		.name = "panel-oneplus-aa553-p-7-a0001",
		.of_match_table = aa553_of_match,
	},
};
module_mipi_dsi_driver(aa553_driver);

MODULE_DESCRIPTION("OnePlus AA553-P-7-A0001 AMOLED DSI panel");
MODULE_LICENSE("GPL");
