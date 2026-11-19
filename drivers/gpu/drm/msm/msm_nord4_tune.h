/* SPDX-License-Identifier: GPL-2.0-only */
/* F0260 follow-up: live geometry knobs, applied at the next enable. */
#ifndef __MSM_NORD4_TUNE_H__
#define __MSM_NORD4_TUNE_H__

#include <linux/types.h>

extern uint nord4_win_2a_start, nord4_win_2a_end;
extern uint nord4_win_2b_start, nord4_win_2b_end;
extern uint nord4_pic_width, nord4_pic_height;
extern uint nord4_slice_width, nord4_slice_height;
extern uint nord4_slice_chunk;
extern uint nord4_slice_per_pkt;
extern uint nord4_eol;
extern uint nord4_widebus;
extern int nord4_h_total_adj;
extern uint nord4_plane_src_x;
extern uint nord4_plane_dst_x;
extern uint nord4_lm_out_pad;

#endif
