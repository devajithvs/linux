// SPDX-License-Identifier: GPL-2.0-only
/*
 * F0260 follow-up: live geometry knobs for the edge-offset question.  Every
 * value defaults to what the driver programs today, so the image is identical
 * to the baseline until a value is changed; each is read at the next enable.
 */
#include <linux/module.h>
#include <linux/kernel.h>

#include "msm_nord4_tune.h"

uint nord4_win_2a_start = 0;
module_param(nord4_win_2a_start, uint, 0644);
MODULE_PARM_DESC(nord4_win_2a_start, "panel 0x2a window start (default 0)");
EXPORT_SYMBOL_GPL(nord4_win_2a_start);

uint nord4_win_2a_end = 1239;
module_param(nord4_win_2a_end, uint, 0644);
MODULE_PARM_DESC(nord4_win_2a_end, "panel 0x2a window end (default 1239)");
EXPORT_SYMBOL_GPL(nord4_win_2a_end);

uint nord4_win_2b_start = 0;
module_param(nord4_win_2b_start, uint, 0644);
MODULE_PARM_DESC(nord4_win_2b_start, "panel 0x2b window start (default 0)");
EXPORT_SYMBOL_GPL(nord4_win_2b_start);

uint nord4_win_2b_end = 2771;
module_param(nord4_win_2b_end, uint, 0644);
MODULE_PARM_DESC(nord4_win_2b_end, "panel 0x2b window end (default 2771)");
EXPORT_SYMBOL_GPL(nord4_win_2b_end);

uint nord4_pic_width = 1240;
module_param(nord4_pic_width, uint, 0644);
MODULE_PARM_DESC(nord4_pic_width, "DSC/PPS picture width (default 1240)");
EXPORT_SYMBOL_GPL(nord4_pic_width);

uint nord4_pic_height = 2772;
module_param(nord4_pic_height, uint, 0644);
MODULE_PARM_DESC(nord4_pic_height, "DSC/PPS picture height (default 2772)");
EXPORT_SYMBOL_GPL(nord4_pic_height);

uint nord4_slice_width = 620;
module_param(nord4_slice_width, uint, 0644);
MODULE_PARM_DESC(nord4_slice_width, "DSC slice width (default 620)");
EXPORT_SYMBOL_GPL(nord4_slice_width);

uint nord4_slice_height = 12;
module_param(nord4_slice_height, uint, 0644);
MODULE_PARM_DESC(nord4_slice_height, "DSC slice height (default 12)");
EXPORT_SYMBOL_GPL(nord4_slice_height);

uint nord4_slice_chunk = 620;
module_param(nord4_slice_chunk, uint, 0644);
MODULE_PARM_DESC(nord4_slice_chunk, "DSC slice chunk size / bytes per line (default 620)");
EXPORT_SYMBOL_GPL(nord4_slice_chunk);

uint nord4_slice_per_pkt = 2;
module_param(nord4_slice_per_pkt, uint, 0644);
MODULE_PARM_DESC(nord4_slice_per_pkt, "DSI slices per packet (default 2)");
EXPORT_SYMBOL_GPL(nord4_slice_per_pkt);

uint nord4_eol = 0xff;
module_param(nord4_eol, uint, 0644);
MODULE_PARM_DESC(nord4_eol, "eol_byte_num 0..3; 0xff = derived (default 0xff)");
EXPORT_SYMBOL_GPL(nord4_eol);

uint nord4_widebus;
module_param(nord4_widebus, uint, 0644);
MODULE_PARM_DESC(nord4_widebus, "0 = version rule, 1 = force on, 2 = force off");
EXPORT_SYMBOL_GPL(nord4_widebus);

int nord4_h_total_adj;
module_param(nord4_h_total_adj, int, 0644);
MODULE_PARM_DESC(nord4_h_total_adj, "added to the DSI compressed h total (default 0)");
EXPORT_SYMBOL_GPL(nord4_h_total_adj);

/* F0261 follow-up: the content-side line-start knobs (command mode has no DSI
 * active-h/h-back-porch/h-skew registers — the DSI programs those only for
 * video mode — so the line start is set by the plane's rects and the LM width).
 */
uint nord4_plane_src_x;
module_param(nord4_plane_src_x, uint, 0644);
MODULE_PARM_DESC(nord4_plane_src_x, "fetch the plane's line this many px in (default 0)");
EXPORT_SYMBOL_GPL(nord4_plane_src_x);

uint nord4_plane_dst_x;
module_param(nord4_plane_dst_x, uint, 0644);
MODULE_PARM_DESC(nord4_plane_dst_x, "move the plane's dst window right by px (default 0)");
EXPORT_SYMBOL_GPL(nord4_plane_dst_x);

uint nord4_lm_out_pad;
module_param(nord4_lm_out_pad, uint, 0644);
MODULE_PARM_DESC(nord4_lm_out_pad, "add px to each LM's output width (default 0)");
EXPORT_SYMBOL_GPL(nord4_lm_out_pad);
