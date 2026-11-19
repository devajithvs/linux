// SPDX-License-Identifier: GPL-2.0-only
/* F0258 follow-up: one switch for the edge-geometry instrumentation. */
#include <linux/module.h>
#include <linux/kernel.h>

#include "msm_nord4_debug.h"

uint nord4_dump;
module_param_named(nord4_dump, nord4_dump, uint, 0644);
MODULE_PARM_DESC(nord4_dump, "1 = dump the DSC/DSI/INTF/LM geometry for the edge-offset question");
EXPORT_SYMBOL_GPL(nord4_dump);
