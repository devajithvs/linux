// SPDX-License-Identifier: GPL-2.0
/*
 * Awinic AW37004 four-channel LDO PMIC
 *
 * Two DVDD outputs (0.6-1.8 V, 6 mV steps) and two AVDD outputs
 * (1.2-4.3 V, 12.5 mV steps), programmed over I2C.
 */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>

#define AW37004_REG_DVDD1_VOUT		0x03
#define AW37004_REG_DVDD2_VOUT		0x04
#define AW37004_REG_AVDD1_VOUT		0x05
#define AW37004_REG_AVDD2_VOUT		0x06
#define AW37004_REG_EN			0x0e
#define AW37004_REG_CHIP_ID2		0x19

#define AW37004_DVDD_VOLT_MIN		600000
#define AW37004_DVDD_VOLT_STEP		6000
#define AW37004_DVDD_VOLT_NUM		201
#define AW37004_AVDD_VOLT_MIN		1200000
#define AW37004_AVDD_VOLT_STEP		12500
#define AW37004_AVDD_VOLT_NUM		249

struct aw37004_regulator {
	struct device *dev;
};

static const struct regulator_ops aw37004_regulator_ops = {
	.enable = regulator_enable_regmap,
	.disable = regulator_disable_regmap,
	.is_enabled = regulator_is_enabled_regmap,
	.list_voltage = regulator_list_voltage_linear,
	.map_voltage = regulator_map_voltage_linear,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
};

#define AW37004_REGULATOR_DESC(_id, _name, _reg, _vmin, _vstep, _vnum)	\
	{								\
		.name = "aw37004-"#_name,				\
		.of_match = of_match_ptr(#_name),			\
		.id = _id,						\
		.ops = &aw37004_regulator_ops,				\
		.type = REGULATOR_VOLTAGE,				\
		.owner = THIS_MODULE,					\
		.vsel_reg = AW37004_REG_##_reg##_VOUT,			\
		.vsel_mask = 0xff,					\
		.enable_reg = AW37004_REG_EN,				\
		.enable_mask = BIT(_id),				\
		.enable_time = 500,					\
		.min_uV = _vmin,					\
		.uV_step = _vstep,					\
		.n_voltages = _vnum,					\
	}

enum {
	AW37004_ID_DVDD1,
	AW37004_ID_DVDD2,
	AW37004_ID_AVDD1,
	AW37004_ID_AVDD2,
};

static const struct regulator_desc aw37004_regulators[] = {
	AW37004_REGULATOR_DESC(AW37004_ID_DVDD1, dvdd1, DVDD1,
			       AW37004_DVDD_VOLT_MIN, AW37004_DVDD_VOLT_STEP,
			       AW37004_DVDD_VOLT_NUM),
	AW37004_REGULATOR_DESC(AW37004_ID_DVDD2, dvdd2, DVDD2,
			       AW37004_DVDD_VOLT_MIN, AW37004_DVDD_VOLT_STEP,
			       AW37004_DVDD_VOLT_NUM),
	AW37004_REGULATOR_DESC(AW37004_ID_AVDD1, avdd1, AVDD1,
			       AW37004_AVDD_VOLT_MIN, AW37004_AVDD_VOLT_STEP,
			       AW37004_AVDD_VOLT_NUM),
	AW37004_REGULATOR_DESC(AW37004_ID_AVDD2, avdd2, AVDD2,
			       AW37004_AVDD_VOLT_MIN, AW37004_AVDD_VOLT_STEP,
			       AW37004_AVDD_VOLT_NUM),
};

static const struct regmap_config aw37004_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = AW37004_REG_CHIP_ID2,
};

static int aw37004_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct aw37004_regulator *chip;
	struct regulator_config config = { };
	struct regmap *regmap;
	int i, ret;

	chip = devm_kzalloc(dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = dev;

	regmap = devm_regmap_init_i2c(client, &aw37004_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap),
				     "Failed to init regmap\n");

	config.dev = dev;
	config.regmap = regmap;
	config.driver_data = chip;

	for (i = 0; i < ARRAY_SIZE(aw37004_regulators); i++) {
		ret = PTR_ERR_OR_ZERO(devm_regulator_register(dev,
							      &aw37004_regulators[i],
							      &config));
		if (ret)
			return dev_err_probe(dev, ret,
					     "Failed to register regulator %s\n",
					     aw37004_regulators[i].name);
	}

	return 0;
}

static const struct i2c_device_id aw37004_id[] = {
	{ .name = "aw37004" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, aw37004_id);

static const struct of_device_id aw37004_of_match[] = {
	{ .compatible = "awinic,aw37004" },
	{ }
};
MODULE_DEVICE_TABLE(of, aw37004_of_match);

static struct i2c_driver aw37004_i2c_driver = {
	.driver = {
		.name = "aw37004-regulator",
		.of_match_table = aw37004_of_match,
	},
	.probe = aw37004_probe,
	.id_table = aw37004_id,
};
module_i2c_driver(aw37004_i2c_driver);

MODULE_DESCRIPTION("Awinic AW37004 regulator driver");
MODULE_AUTHOR("Devajith V S");
MODULE_LICENSE("GPL");
