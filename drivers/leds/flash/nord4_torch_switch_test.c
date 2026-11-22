// SPDX-License-Identifier: GPL-2.0-only
/*
 * Test-only helper for the attended torch check (F0387): read the flash LED
 * switch register through the PMIC's regmap, and when asked set or clear its
 * bit 0 on a running kernel. The daily's driver never writes this byte and the
 * kernel's regmap debugfs is read-only, so a module is the only userspace
 * reachable way in; nothing selects this file but an explicit
 * `--modules drivers/leds/flash/nord4_torch_switch_test.ko`.
 *
 * The flash node is a platform child of the PMIC MFD, exactly as the driver
 * sees it; the register addresses are reg_base + offset on the parent regmap.
 *
 *   modprobe nord4-torch-switch-test          dump 0x65/0x46/0x4e, change nothing
 *   modprobe nord4-torch-switch-test enable=1 switch on (0x65 bit 0 set)
 *   modprobe nord4-torch-switch-test enable=-1 switch off (0x65 bit 0 clear)
 *   rmmod                                     restore the value found at load
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define SWITCH_REG	0x65
#define MOD_REG		0x46
#define CHAN_REG	0x4e

static int enable;
module_param(enable, int, 0444);
MODULE_PARM_DESC(enable, "0 = dump only (default), 1 = switch on, -1 = switch off");

static struct regmap *regmap;
static u32 base;
static unsigned int orig;
static bool changed;

static void dump(const char *tag);

/* Write anything to this parameter to dump the registers at that moment
 * (used while a torch is on, when the LED enable registers are not zero).
 */
static int read_now_set(const char *val, const struct kernel_param *kp)
{
	dump("now");
	return 0;
}
static const struct kernel_param_ops read_now_ops = {
	.set = read_now_set,
};
module_param_cb(read_now, &read_now_ops, NULL, 0200);

static void dump_map(const char *tag)
{
	char line[160];
	unsigned int v;
	int r, n;

	for (r = 0; r < 0x80; r += 8) {
		n = scnprintf(line, sizeof(line), "nord4-torch-switch: %s %#04x:", tag, base + r);
		for (int k = 0; k < 8; k++) {
			if (regmap_read(regmap, base + r + k, &v))
				v = 0xff;
			n += scnprintf(line + n, sizeof(line) - n, " %02x", v & 0xff);
		}
		pr_info("%s\n", line);
	}
}

static void dump(const char *tag)
{
	unsigned int sw = 0, mod = 0, ch = 0;

	if (regmap_read(regmap, base + SWITCH_REG, &sw) ||
	    regmap_read(regmap, base + MOD_REG, &mod) ||
	    regmap_read(regmap, base + CHAN_REG, &ch)) {
		pr_err("nord4-torch-switch: read failed, base %#x\n", base);
		return;
	}
	pr_info("nord4-torch-switch: %s switch(%#x)=%#02x mod(%#x)=%#02x chan(%#x)=%#02x\n",
		tag, SWITCH_REG, sw & 0xff, MOD_REG, mod & 0xff, CHAN_REG, ch & 0xff);
	dump_map(tag);
}

static int __init nord4_torch_switch_init(void)
{
	struct device_node *np;
	struct platform_device *pdev;
	unsigned int want;
	int rc;

	np = of_find_compatible_node(NULL, NULL, "qcom,spmi-flash-led");
	if (!np)
		np = of_find_compatible_node(NULL, NULL, "qcom,pm7550-flash-led");
	if (!np)
		np = of_find_compatible_node(NULL, NULL, "qcom,pm8350c-flash-led");
	if (!np) {
		pr_err("nord4-torch-switch: no flash-led node\n");
		return -ENODEV;
	}
	rc = of_property_read_u32(np, "reg", &base);
	if (rc) {
		pr_err("nord4-torch-switch: no reg property: %d\n", rc);
		of_node_put(np);
		return rc;
	}
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev) {
		pr_err("nord4-torch-switch: no platform device\n");
		return -ENODEV;
	}
	regmap = dev_get_regmap(pdev->dev.parent, NULL);
	put_device(&pdev->dev);
	if (!regmap) {
		pr_err("nord4-torch-switch: no parent regmap\n");
		return -ENODEV;
	}

	if (regmap_read(regmap, base + SWITCH_REG, &orig)) {
		pr_err("nord4-torch-switch: cannot read switch\n");
		return -EIO;
	}
	dump("before");

	if (enable) {
		want = (orig & ~BIT(0));
		if (enable > 0)
			want |= BIT(0);
		rc = regmap_write(regmap, base + SWITCH_REG, want);
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
	if (regmap_write(regmap, base + SWITCH_REG, orig))
		pr_err("nord4-torch-switch: restore failed\n");
	else
		dump("restored");
}

module_init(nord4_torch_switch_init);
module_exit(nord4_torch_switch_exit);

MODULE_DESCRIPTION("Test-only flash LED switch access (F0387)");
MODULE_LICENSE("GPL");
