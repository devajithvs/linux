// SPDX-License-Identifier: GPL-2.0-only
/*
 * Test-only helper for the attended torch check (F0387): read the pm8350c
 * flash LED switch register, and when asked, set or clear its bit 0 on a
 * running kernel. The daily's driver never writes this byte, so a module is
 * the only userspace-reachable way in; this file is built only by an explicit
 * `--modules drivers/leds/flash/nord4_torch_switch_test.ko` and no Kconfig
 * symbol selects it, so no product image carries it.
 *
 *   modprobe nord4-torch-switch-test          dump 0x65/0x46/0x4e, change nothing
 *   modprobe nord4-torch-switch-test enable=1 switch on (0x65 bit 0 set)
 *   modprobe nord4-torch-switch-test enable=-1 switch off (0x65 bit 0 clear)
 *   rmmod                                     restore the value found at load
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/spmi.h>

#define SWITCH_REG	0x65
#define MOD_REG		0x46
#define CHAN_REG	0x4e

static int enable = 0;
module_param(enable, int, 0444);
MODULE_PARM_DESC(enable, "0 = dump only (default), 1 = switch on, -1 = switch off");

static struct spmi_device *sdev;
static u16 base;
static u8 orig;
static bool changed;

static int read8(u32 reg, u8 *val)
{
	return spmi_ext_register_read(sdev, base + reg, val, 1);
}

static void dump(const char *tag)
{
	u8 sw = 0, mod = 0, ch = 0;

	if (read8(SWITCH_REG, &sw) || read8(MOD_REG, &mod) || read8(CHAN_REG, &ch)) {
		pr_err("nord4-torch-switch: read failed, sdev %s base %#x\n",
		       dev_name(&sdev->dev), base);
		return;
	}
	pr_info("nord4-torch-switch: %s switch(%#x)=%#02x mod(%#x)=%#02x chan(%#x)=%#02x\n",
		tag, SWITCH_REG, sw, MOD_REG, mod, CHAN_REG, ch);
}

static int __init nord4_torch_switch_init(void)
{
	struct device_node *np;
	u8 v;
	int rc;

	np = of_find_compatible_node(NULL, NULL, "qcom,pm8350c-flash-led");
	if (!np) {
		pr_err("nord4-torch-switch: no pm8350c flash node\n");
		return -ENODEV;
	}
	rc = of_property_read_u16(np, "reg", &base);
	if (rc) {
		pr_err("nord4-torch-switch: no reg property: %d\n", rc);
		of_node_put(np);
		return rc;
	}
	sdev = spmi_find_device_by_of_node(np);
	of_node_put(np);
	if (!sdev) {
		pr_err("nord4-torch-switch: no spmi device\n");
		return -ENODEV;
	}

	if (read8(SWITCH_REG, &orig)) {
		pr_err("nord4-torch-switch: cannot read switch\n");
		return -EIO;
	}
	dump("before");

	if (enable) {
		v = enable > 0 ? (orig | BIT(0)) : (orig & ~BIT(0));
		rc = spmi_ext_register_write(sdev, base + SWITCH_REG, &v, 1);
		if (rc) {
			pr_err("nord4-torch-switch: write failed: %d\n", rc);
			return rc;
		}
		changed = true;
		dump(enable > 0 ? "switch on" : "switch off");
	}
	return 0;
}

static void __exit nord4_torch_switch_exit(void)
{
	if (!changed)
		return;
	if (spmi_ext_register_write(sdev, base + SWITCH_REG, &orig, 1))
		pr_err("nord4-torch-switch: restore failed\n");
	else
		dump("restored");
}

module_init(nord4_torch_switch_init);
module_exit(nord4_torch_switch_exit);

MODULE_DESCRIPTION("Test-only pm8350c flash LED switch access (F0387)");
MODULE_LICENSE("GPL");
