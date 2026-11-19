// SPDX-License-Identifier: GPL-2.0-only
/*
 * F0263/F0271 follow-up: the live discriminators of the content-shift question.
 * All are 0644 and read at the next enable; with their defaults no write is
 * made and the only output is nothing, so the image equals the clean baseline.
 */
#include <linux/kernel.h>
#include <linux/module.h>

#include "msm_nord4_geo.h"

uint nord4_dsc_dump;
module_param(nord4_dsc_dump, uint, 0644);
MODULE_PARM_DESC(nord4_dsc_dump,
	"1 = print the INTF/DSI/PPS state and the kickoff/pp-done/TE counts (default 0)");
EXPORT_SYMBOL_GPL(nord4_dsc_dump);

int nord4_intf_data_hctl = -1;
module_param(nord4_intf_data_hctl, int, 0644);
MODULE_PARM_DESC(nord4_intf_data_hctl,
	"INTF_DISPLAY_DATA_HCTL start in pclk; end = start + 206; -1 = write nothing (default -1)");
EXPORT_SYMBOL_GPL(nord4_intf_data_hctl);

uint nord4_dd_wr_hs;
module_param(nord4_dd_wr_hs, uint, 0644);
MODULE_PARM_DESC(nord4_dd_wr_hs,
	"1 = re-send DCS 0x2a/0x2b in HS after 0x29 (default 0)");
EXPORT_SYMBOL_GPL(nord4_dd_wr_hs);

int nord4_dd17 = -1;
module_param(nord4_dd17, int, 0644);
MODULE_PARM_DESC(nord4_dd17,
	"page-1 0x17 value written after 0x29; -1 = write nothing (default -1)");
EXPORT_SYMBOL_GPL(nord4_dd17);

uint nord4_split_fetch;
module_param(nord4_split_fetch, uint, 0644);
MODULE_PARM_DESC(nord4_split_fetch,
	"1 = split the full-width plane into one VIG pipe per LM (src x 0/620, out x 0); default 0");
EXPORT_SYMBOL_GPL(nord4_split_fetch);

uint nord4_dsi_dst_fmt;
module_param(nord4_dsi_dst_fmt, uint, 0644);
MODULE_PARM_DESC(nord4_dsi_dst_fmt,
	"1 = DSI_CMD_CFG0 = the bootloader's 0x06100008 (RGB888, interleave 1); default 0");
EXPORT_SYMBOL_GPL(nord4_dsi_dst_fmt);

int nord4_dsc_initial_lines = -1;
module_param(nord4_dsc_initial_lines, int, 0644);
MODULE_PARM_DESC(nord4_dsc_initial_lines,
	"DSC ENC_DF_CTRL initial_lines and bit 12 (the bootloader's 3); -1 = keep the computed value");
EXPORT_SYMBOL_GPL(nord4_dsc_initial_lines);
