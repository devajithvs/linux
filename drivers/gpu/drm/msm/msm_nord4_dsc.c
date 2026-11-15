// SPDX-License-Identifier: GPL-2.0-only
/*
 * HACK: runtime knobs for the AA553 panel's DSC bring-up (F0151, F0153).
 *
 * One boot, many looks: the DSI host and the panel driver read these at every
 * mode set and every panel prepare, so a userspace sweep changes one
 * hypothesis between two mode sets instead of one hypothesis per build.
 * Every knob defaults to 0, the behaviour of the tree before this file.
 */

#include <linux/module.h>
#include <linux/kernel.h>

#include "msm_nord4_dsc.h"

uint nord4_dsc_slice_per_pkt;
module_param_named(nord4_dsc_slice_per_pkt, nord4_dsc_slice_per_pkt, uint, 0644);
MODULE_PARM_DESC(nord4_dsc_slice_per_pkt, "0 = panel value, 1 or 2 = force");
EXPORT_SYMBOL_GPL(nord4_dsc_slice_per_pkt);

uint nord4_dsc_rc_table;
module_param_named(nord4_dsc_rc_table, nord4_dsc_rc_table, uint, 0644);
MODULE_PARM_DESC(nord4_dsc_rc_table, "0 = by DSC version, 1 = DSC 1.1 pre-SCR, 2 = DSC 1.2 4:4:4");
EXPORT_SYMBOL_GPL(nord4_dsc_rc_table);

uint nord4_dsc_page0;
module_param_named(nord4_dsc_page0, nord4_dsc_page0, uint, 0644);
MODULE_PARM_DESC(nord4_dsc_page0, "1 = write the vendor table's page-0 select before the first 0xb4");
EXPORT_SYMBOL_GPL(nord4_dsc_page0);

uint nord4_dsc_pps_when;
module_param_named(nord4_dsc_pps_when, nord4_dsc_pps_when, uint, 0644);
MODULE_PARM_DESC(nord4_dsc_pps_when, "0 = after display-on, 1 = before sleep-out, 2 = before display-on, 3 = also in enable");
EXPORT_SYMBOL_GPL(nord4_dsc_pps_when);

uint nord4_dsc_comp_dcs;
module_param_named(nord4_dsc_comp_dcs, nord4_dsc_comp_dcs, uint, 0644);
MODULE_PARM_DESC(nord4_dsc_comp_dcs, "1 = do not send the DCS 0x07 set-compression-mode write");
EXPORT_SYMBOL_GPL(nord4_dsc_comp_dcs);

uint nord4_dsc_bpp;
module_param_named(nord4_dsc_bpp, nord4_dsc_bpp, uint, 0644);
MODULE_PARM_DESC(nord4_dsc_bpp, "0 = panel value (0x80), else raw bits_per_pixel in 4.4 fixed point");
EXPORT_SYMBOL_GPL(nord4_dsc_bpp);

uint nord4_dsc_stream;
module_param_named(nord4_dsc_stream, nord4_dsc_stream, uint, 0644);
MODULE_PARM_DESC(nord4_dsc_stream, "0 = today, 1 = wc line+1, 2 = wc slice+1, 3 = pkt/line = slices, 4 = pkt/line = 1");
EXPORT_SYMBOL_GPL(nord4_dsc_stream);

int nord4_dsc_eol = -1;
module_param_named(nord4_dsc_eol, nord4_dsc_eol, int, 0644);
MODULE_PARM_DESC(nord4_dsc_eol, "-1 = today, else 0..3 eol_byte_num");
EXPORT_SYMBOL_GPL(nord4_dsc_eol);

uint nord4_dsc_widebus;
module_param_named(nord4_dsc_widebus, nord4_dsc_widebus, uint, 0644);
MODULE_PARM_DESC(nord4_dsc_widebus, "0 = host version rule, 1 = force on, 2 = force off");
EXPORT_SYMBOL_GPL(nord4_dsc_widebus);

uint nord4_dsc_merge;
module_param_named(nord4_dsc_merge, nord4_dsc_merge, uint, 0644);
MODULE_PARM_DESC(nord4_dsc_merge, "0 = topology, 1 = force DSC_MODE_MULTIPLEX on, 2 = force it off");
EXPORT_SYMBOL_GPL(nord4_dsc_merge);

uint nord4_dsc_readback;
module_param_named(nord4_dsc_readback, nord4_dsc_readback, uint, 0644);
MODULE_PARM_DESC(nord4_dsc_readback, "1 = read the DDIC's status registers at every unprepare");
EXPORT_SYMBOL_GPL(nord4_dsc_readback);

uint nord4_dsc_dump;
module_param_named(nord4_dsc_dump, nord4_dsc_dump, uint, 0644);
MODULE_PARM_DESC(nord4_dsc_dump, "1 = dump the DSI/DPU/DSC registers at every mode set");
EXPORT_SYMBOL_GPL(nord4_dsc_dump);
