/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Command-mode line-origin diagnostics: the INTF compressed-data window, the
 * DDIC's column window, the panel's page-1 0x17, the bootloader's two-pipe
 * line fetch, its DSI CMD_CFG0 word and its DSC initial_lines, plus the
 * register dump and the kickoff/pp-done counters that decide whether each one
 * is in the path (F0263, F0271, docs/plans/panel-geometry.md §1, §9).
 *
 * Every default leaves the driver programming exactly what nord4/display-clean
 * programs; the values are read at the next enable.
 */
#ifndef __MSM_NORD4_GEO_H__
#define __MSM_NORD4_GEO_H__

#include <linux/types.h>

extern uint nord4_dsc_dump;
extern int nord4_intf_data_hctl;
extern uint nord4_dd_wr_hs;
extern int nord4_dd17;
extern uint nord4_split_fetch;
extern uint nord4_dsi_dst_fmt;
extern int nord4_dsc_initial_lines;

#endif /* __MSM_NORD4_GEO_H__ */
