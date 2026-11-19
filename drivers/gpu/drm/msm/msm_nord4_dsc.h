/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * HACK: runtime knobs for the AA553 panel's DSC bring-up (F0151, F0153).
 *
 * One boot, many looks: the values are read at every panel prepare and every
 * DSI host mode set, so a userspace sweep can change a hypothesis between two
 * mode sets without rebuilding.  Every knob defaults to 0, which is the
 * behaviour of the tree before this file existed.
 *
 * These knobs are a measurement aid and go away once the picture is right.
 */
#ifndef __MSM_NORD4_DSC_H__
#define __MSM_NORD4_DSC_H__

#include <linux/types.h>

/* 0 = the panel's qcom,mdss-dsc-slice-per-pkt (2), 1 or 2 = force it */
extern uint nord4_dsc_slice_per_pkt;
/* 0 = pick by dsc_version_minor (1.2 today), 1 = DSC 1.1 pre-SCR, 2 = DSC 1.2 4:4:4 */
extern uint nord4_dsc_rc_table;
/* 0 = no extra write, 1 = the vendor table's page-0 select between 0xba and the first 0xb4 */
/* 0 = after display-on (today), 1 = before sleep-out, 2 = before display-on, 3 = again in panel enable */
extern uint nord4_dsc_pps_when;
/* 0 = send the DCS 0x07 set-compression-mode write (today), 1 = do not send it */
/* 0 = the panel's 8 bpp (0x80 in 4.4 fixed point), else that raw value */
extern uint nord4_dsc_bpp;
/* 0 = today's stream registers, 1 = wc = line bytes + 1, 2 = wc = slice bytes + 1,
 * 3 = packets per line = slice count, 4 = packets per line = 1 */
extern uint nord4_dsc_stream;
/* -1 = today's (total bytes per interface % 3), else 0..3 */
extern int nord4_dsc_eol;
/* 0 = the host's version rule (on today), 1 = force widebus on, 2 = force it off */
extern uint nord4_dsc_widebus;
/* 0 = the topology's answer (multiplex on today), 1 = force DSC_MODE_MULTIPLEX on,
 * 2 = force it off */
extern uint nord4_dsc_merge;
/* 0 = no readback, 1 = read the DDIC's status registers at every panel unprepare */
extern uint nord4_dsc_readback;
/* 0 = quiet, 1 = dump the DSI/DPU/DSC registers at every mode set */
extern uint nord4_dsc_dump;
/* 0 = quiet, 1 = dump the plane's pipes, every SSPP's SRC0-3 read back and
 * the CTL's active registers at every plane update and every CTL flush
 */
extern uint nord4_sspp_dump;
/* 0 = the tree's behaviour, 1 = re-program every visible plane of the CRTC at
 * the command encoder's enable (re-enable fix candidate)
 */
extern uint nord4_reenable_rearm;
extern uint nord4_ctl_reset_on_enable;
extern uint nord4_aa553_w1_reset_hold_us;
extern uint nord4_aa553_w2_reset_settle_us;
extern uint nord4_aa553_w3_pps_ms;
extern uint nord4_aa553_w4_sleepout_ms;

#endif
