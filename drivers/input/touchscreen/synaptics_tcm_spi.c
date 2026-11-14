// SPDX-License-Identifier: GPL-2.0-only
/*
 * Synaptics TouchComm (TCM) generation 1 touchscreen controller over SPI.
 *
 * This is a minimal driver for the Synaptics S3910 in the OnePlus Nord 4
 * (Avalon). It speaks the vendor's TouchComm v1 packet format directly over
 * SPI: commands are a command byte, a 16-bit little-endian payload length,
 * the payload and a CRC-16; responses and reports are prefixed with the 0xa5
 * marker and a 16-bit length.
 *
 * The downstream OnePlus stack splits the same protocol over several modules
 * (syna_tcm2_platform_spi.c, synaptics_touchcom_core_v1.c and
 * synaptics_touchcom_func_touch.c); this driver keeps only the parts needed
 * to identify the controller, fetch its touch report layout and report
 * multitouch contacts.
 *
 * Untested: the protocol constants and the power/reset timings are taken
 * from the downstream sources, not from an observed bus trace.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>
#include <linux/unaligned.h>

#define SYNA_TCM_MARKER			0xa5
#define SYNA_TCM_HEADER_SIZE		4
#define SYNA_TCM_CRC_SIZE		2
#define SYNA_TCM_MAX_FRAME		2048
#define SYNA_TCM_MAX_OBJECTS		10

/* Downstream timings: synaptics,power-delay-ms/reset-active-ms/reset-delay-ms */
#define SYNA_TCM_POWER_ON_MS		200
/*
 * The downstream driver targets 3.1 V, but this rail's PM7550 pldo law is
 * 1.504 V + n * 8 mV (drivers/regulator/qcom-rpmh-regulator.c:780-788),
 * so 3.1 V is not a reachable step. Ask for the nearest step at or below
 * it; the board's 2.7-3.4 V constraints are unchanged.
 */
#define SYNA_TCM_VDD_UV		3096000
#define SYNA_TCM_RESET_ACTIVE_MS	10
#define SYNA_TCM_RESET_DELAY_MS		80
/* Bus turnaround after a command write, TAT_DELAY_US_MIN downstream */
#define SYNA_TCM_TAT_US			100

enum syna_tcm_command {
	SYNA_TCM_CMD_IDENTIFY			= 0x02,
	SYNA_TCM_CMD_GET_APPLICATION_INFO	= 0x20,
	SYNA_TCM_CMD_GET_TOUCH_REPORT_CONFIG	= 0x25,
};

enum syna_tcm_status {
	SYNA_TCM_STATUS_OK		= 0x01,
	SYNA_TCM_STATUS_CONTINUED_READ	= 0x03,
	SYNA_TCM_STATUS_ERROR		= 0x0f,
	SYNA_TCM_REPORT_IDENTIFY	= 0x10,
	SYNA_TCM_REPORT_TOUCH		= 0x11,
};

enum syna_tcm_dev_mode {
	SYNA_TCM_MODE_APPLICATION = 0x01,
};

enum syna_tcm_touch_report_code {
	SYNA_TCM_TOUCH_END			= 0x00,
	SYNA_TCM_TOUCH_FOREACH_ACTIVE_OBJECT	= 0x01,
	SYNA_TCM_TOUCH_FOREACH_OBJECT		= 0x02,
	SYNA_TCM_TOUCH_FOREACH_END		= 0x03,
	SYNA_TCM_TOUCH_PAD_TO_NEXT_BYTE		= 0x04,
	SYNA_TCM_TOUCH_TIMESTAMP		= 0x05,
	SYNA_TCM_TOUCH_OBJECT_N_INDEX		= 0x06,
	SYNA_TCM_TOUCH_OBJECT_N_CLASSIFICATION	= 0x07,
	SYNA_TCM_TOUCH_OBJECT_N_X_POSITION	= 0x08,
	SYNA_TCM_TOUCH_OBJECT_N_Y_POSITION	= 0x09,
	SYNA_TCM_TOUCH_OBJECT_N_Z		= 0x0a,
	SYNA_TCM_TOUCH_OBJECT_N_X_WIDTH		= 0x0b,
	SYNA_TCM_TOUCH_OBJECT_N_Y_WIDTH		= 0x0c,
	SYNA_TCM_TOUCH_NUM_OF_ACTIVE_OBJECTS	= 0x18,
};

enum syna_tcm_object_status {
	SYNA_TCM_OBJECT_LIFT	= 0x00,
	SYNA_TCM_OBJECT_FINGER	= 0x01,
	SYNA_TCM_OBJECT_GLOVE	= 0x02,
};

struct syna_tcm_object {
	u8 status;
	unsigned int x;
	unsigned int y;
	unsigned int z;
	unsigned int x_width;
	unsigned int y_width;
};

struct syna_tcm {
	struct spi_device *spi;
	struct input_dev *input;
	struct gpio_desc *reset_gpio;
	struct regulator *vdd;
	struct mutex io_lock;
	u8 *tx;
	u8 *rx;
	u8 report_config[256];
	unsigned int report_config_len;
	unsigned int max_x;
	unsigned int max_y;
	unsigned int max_objects;
};

static u16 syna_tcm_crc16(const u8 *data, unsigned int len, u16 crc)
{
	unsigned int bit;

	while (len--) {
		crc ^= (u16)*data++ << 8;
		for (bit = 0; bit < 8; bit++)
			crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021
					     : crc << 1;
	}

	return crc;
}

static int syna_tcm_send(struct syna_tcm *tcm, u8 command,
			 const u8 *payload, unsigned int payload_len)
{
	unsigned int len = 3 + payload_len + SYNA_TCM_CRC_SIZE;
	u16 crc;

	if (payload_len > SYNA_TCM_MAX_FRAME - 5)
		return -EINVAL;

	tcm->tx[0] = command;
	tcm->tx[1] = payload_len & 0xff;
	tcm->tx[2] = payload_len >> 8;
	memcpy(&tcm->tx[3], payload, payload_len);

	crc = syna_tcm_crc16(tcm->tx, 3 + payload_len, 0xffff);
	tcm->tx[3 + payload_len] = crc & 0xff;
	tcm->tx[4 + payload_len] = crc >> 8;

	return spi_write(tcm->spi, tcm->tx, len);
}

static int syna_tcm_receive(struct syna_tcm *tcm, u8 *code,
			    unsigned int *payload_len)
{
	struct spi_transfer xfer = {
		.tx_buf = tcm->tx,
		.rx_buf = tcm->rx,
		.len = SYNA_TCM_MAX_FRAME,
	};
	struct spi_message msg;
	unsigned int len;
	int err;

	memset(tcm->tx, 0xff, SYNA_TCM_MAX_FRAME);

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);

	err = spi_sync(tcm->spi, &msg);
	if (err)
		return err;

	if (tcm->rx[0] != SYNA_TCM_MARKER)
		return -EPROTO;

	*code = tcm->rx[1];
	len = get_unaligned_le16(&tcm->rx[2]);
	if (len > SYNA_TCM_MAX_FRAME - SYNA_TCM_HEADER_SIZE)
		return -EPROTO;

	*payload_len = len;

	return 0;
}

/*
 * Exchange one command and copy its response payload. The caller's buffer
 * keeps the payload valid after io_lock is dropped; reports arriving in the
 * interrupt thread are read from the same transfer buffers.
 */
static int syna_tcm_cmd(struct syna_tcm *tcm, u8 command, u8 *code,
			u8 *resp, unsigned int resp_size,
			unsigned int *resp_len)
{
	unsigned int len;
	int err;

	mutex_lock(&tcm->io_lock);

	err = syna_tcm_send(tcm, command, NULL, 0);
	if (err)
		goto out_unlock;

	usleep_range(SYNA_TCM_TAT_US, SYNA_TCM_TAT_US + 100);

	err = syna_tcm_receive(tcm, code, &len);
	if (err)
		goto out_unlock;

	if (resp && resp_size)
		memcpy(resp, &tcm->rx[SYNA_TCM_HEADER_SIZE],
		       min(len, resp_size));
	if (resp_len)
		*resp_len = len;

out_unlock:
	mutex_unlock(&tcm->io_lock);

	return err;
}

static int syna_tcm_identify(struct syna_tcm *tcm)
{
	u8 id[64];
	unsigned int len;
	u8 code;
	int err;

	err = syna_tcm_cmd(tcm, SYNA_TCM_CMD_IDENTIFY, &code,
			   id, sizeof(id), &len);
	if (err)
		return err;

	if (code != SYNA_TCM_REPORT_IDENTIFY || len < 2)
		return -ENODEV;

	/* struct tcm_identification_info: version, mode, part number, ... */
	if (id[1] != SYNA_TCM_MODE_APPLICATION)
		return -ENODEV;

	dev_info(&tcm->spi->dev, "S3910 in application mode, part %.16s\n",
		 &id[2]);

	return 0;
}

static int syna_tcm_get_app_info(struct syna_tcm *tcm)
{
	u8 info[64];
	unsigned int len;
	u8 code;
	int err;

	err = syna_tcm_cmd(tcm, SYNA_TCM_CMD_GET_APPLICATION_INFO, &code,
			   info, sizeof(info), &len);
	if (err)
		return err;

	if (code != SYNA_TCM_STATUS_OK || len < 38)
		return -EIO;

	if (get_unaligned_le16(&info[2]) != 0) {
		dev_err(&tcm->spi->dev, "application status 0x%04x\n",
			get_unaligned_le16(&info[2]));
		return -ENODEV;
	}

	tcm->max_x = get_unaligned_le16(&info[32]);
	tcm->max_y = get_unaligned_le16(&info[34]);
	tcm->max_objects = get_unaligned_le16(&info[36]);

	dev_info(&tcm->spi->dev, "%ux%u, %u objects\n",
		 tcm->max_x, tcm->max_y, tcm->max_objects);

	if (!tcm->max_x || !tcm->max_y || !tcm->max_objects)
		return -ENODEV;

	tcm->max_objects = min_t(unsigned int, tcm->max_objects,
				 SYNA_TCM_MAX_OBJECTS);

	return 0;
}

static int syna_tcm_get_report_config(struct syna_tcm *tcm)
{
	u8 code;
	int err;

	err = syna_tcm_cmd(tcm, SYNA_TCM_CMD_GET_TOUCH_REPORT_CONFIG, &code,
			   tcm->report_config, sizeof(tcm->report_config),
			   &tcm->report_config_len);
	if (err)
		return err;

	if (code != SYNA_TCM_STATUS_OK || !tcm->report_config_len)
		return -EIO;

	return 0;
}

static int syna_tcm_get_field(const u8 *report, unsigned int report_len,
			      unsigned int offset, unsigned int bits,
			      unsigned int *value)
{
	unsigned int bit_offset = offset % 8;
	unsigned int byte_offset = offset / 8;
	unsigned int remaining = bits;
	unsigned int result = 0;

	if (!bits || bits > 32)
		return -EINVAL;

	if (offset + bits > report_len * 8) {
		*value = 0;
		return 0;
	}

	while (remaining) {
		unsigned int available = 8 - bit_offset;
		unsigned int take = min(available, remaining);
		u8 byte = report[byte_offset] >> bit_offset;

		byte &= 0xff >> (8 - take);
		result |= byte << (bits - remaining);

		bit_offset = 0;
		byte_offset++;
		remaining -= take;
	}

	*value = result;

	return 0;
}

static void syna_tcm_report_touch(struct syna_tcm *tcm, const u8 *report,
				  unsigned int report_len)
{
	struct syna_tcm_object objects[SYNA_TCM_MAX_OBJECTS] = { };
	unsigned int active_objects = 0;
	bool active_known = false, active_only = false;
	unsigned int objects_seen = 0;
	unsigned int config_idx = 0;
	unsigned int foreach_start = 0;
	unsigned int offset = 0, obj = 0;
	unsigned int bits, value;
	int err;

	while (config_idx < tcm->report_config_len) {
		u8 code = tcm->report_config[config_idx++];

		switch (code) {
		case SYNA_TCM_TOUCH_END:
			goto report;
		case SYNA_TCM_TOUCH_FOREACH_ACTIVE_OBJECT:
		case SYNA_TCM_TOUCH_FOREACH_OBJECT:
			active_only = code == SYNA_TCM_TOUCH_FOREACH_ACTIVE_OBJECT;
			obj = 0;
			foreach_start = config_idx;
			break;
		case SYNA_TCM_TOUCH_FOREACH_END:
			obj++;
			if (active_only) {
				if (active_known) {
					objects_seen++;
					if (objects_seen < active_objects)
						config_idx = foreach_start;
				} else if (offset < report_len * 8) {
					config_idx = foreach_start;
				}
			} else if (obj < tcm->max_objects) {
				config_idx = foreach_start;
			}
			break;
		case SYNA_TCM_TOUCH_PAD_TO_NEXT_BYTE:
			offset = (offset + 7) & ~7;
			break;
		default:
			if (config_idx >= tcm->report_config_len)
				goto report;
			bits = tcm->report_config[config_idx++];

			switch (code) {
			case SYNA_TCM_TOUCH_NUM_OF_ACTIVE_OBJECTS:
				err = syna_tcm_get_field(report, report_len,
							 offset, bits, &value);
				if (!err) {
					active_objects = value;
					active_known = true;
				}
				offset += bits;
				break;
			case SYNA_TCM_TOUCH_OBJECT_N_INDEX:
				err = syna_tcm_get_field(report, report_len,
							 offset, bits, &value);
				if (!err)
					obj = value;
				offset += bits;
				break;
			case SYNA_TCM_TOUCH_OBJECT_N_CLASSIFICATION:
				err = syna_tcm_get_field(report, report_len,
							 offset, bits, &value);
				if (!err && obj < tcm->max_objects)
					objects[obj].status = value;
				offset += bits;
				break;
			case SYNA_TCM_TOUCH_OBJECT_N_X_POSITION:
				err = syna_tcm_get_field(report, report_len,
							 offset, bits, &value);
				if (!err && obj < tcm->max_objects)
					objects[obj].x = value;
				offset += bits;
				break;
			case SYNA_TCM_TOUCH_OBJECT_N_Y_POSITION:
				err = syna_tcm_get_field(report, report_len,
							 offset, bits, &value);
				if (!err && obj < tcm->max_objects)
					objects[obj].y = value;
				offset += bits;
				break;
			case SYNA_TCM_TOUCH_OBJECT_N_Z:
				err = syna_tcm_get_field(report, report_len,
							 offset, bits, &value);
				if (!err && obj < tcm->max_objects)
					objects[obj].z = value;
				offset += bits;
				break;
			case SYNA_TCM_TOUCH_OBJECT_N_X_WIDTH:
				err = syna_tcm_get_field(report, report_len,
							 offset, bits, &value);
				if (!err && obj < tcm->max_objects)
					objects[obj].x_width = value;
				offset += bits;
				break;
			case SYNA_TCM_TOUCH_OBJECT_N_Y_WIDTH:
				err = syna_tcm_get_field(report, report_len,
							 offset, bits, &value);
				if (!err && obj < tcm->max_objects)
					objects[obj].y_width = value;
				offset += bits;
				break;
			default:
				/* Every other report code carries a bit length. */
				offset += bits;
				break;
			}
			break;
		}
	}

report:
	for (obj = 0; obj < tcm->max_objects; obj++) {
		struct syna_tcm_object *object = &objects[obj];
		bool pressed = object->status == SYNA_TCM_OBJECT_FINGER ||
			       object->status == SYNA_TCM_OBJECT_GLOVE;
		unsigned int major = max(object->x_width, object->y_width);
		unsigned int minor = min(object->x_width, object->y_width);

		input_mt_slot(tcm->input, obj);
		input_mt_report_slot_state(tcm->input, MT_TOOL_FINGER,
					   pressed);
		if (!pressed)
			continue;

		input_report_abs(tcm->input, ABS_MT_POSITION_X, object->x);
		input_report_abs(tcm->input, ABS_MT_POSITION_Y, object->y);
		input_report_abs(tcm->input, ABS_MT_TOUCH_MAJOR, major);
		input_report_abs(tcm->input, ABS_MT_TOUCH_MINOR, minor);
		input_report_abs(tcm->input, ABS_MT_PRESSURE, object->z);
	}

	input_sync(tcm->input);
}

static irqreturn_t syna_tcm_irq(int irq, void *data)
{
	struct syna_tcm *tcm = data;
	unsigned int len;
	u8 code;
	int err;

	mutex_lock(&tcm->io_lock);
	err = syna_tcm_receive(tcm, &code, &len);
	mutex_unlock(&tcm->io_lock);

	if (err) {
		dev_warn_ratelimited(&tcm->spi->dev, "read failed: %d\n", err);
		return IRQ_HANDLED;
	}

	if (code == SYNA_TCM_REPORT_TOUCH && len)
		syna_tcm_report_touch(tcm, &tcm->rx[SYNA_TCM_HEADER_SIZE], len);

	return IRQ_HANDLED;
}

static int syna_tcm_power_on(struct syna_tcm *tcm)
{
	int err;

	err = regulator_enable(tcm->vdd);
	if (err)
		return err;

	if (regulator_count_voltages(tcm->vdd) > 0) {
		err = regulator_set_voltage(tcm->vdd, SYNA_TCM_VDD_UV,
					    SYNA_TCM_VDD_UV);
		if (err)
			goto err_disable;
	}

	/* The controller samples its reset line only after the rail is up. */
	msleep(SYNA_TCM_POWER_ON_MS);

	gpiod_set_value_cansleep(tcm->reset_gpio, 1);
	msleep(SYNA_TCM_RESET_ACTIVE_MS);
	gpiod_set_value_cansleep(tcm->reset_gpio, 0);
	msleep(SYNA_TCM_RESET_DELAY_MS);

	return 0;

err_disable:
	regulator_disable(tcm->vdd);

	return err;
}

static int syna_tcm_probe(struct spi_device *spi)
{
	struct syna_tcm *tcm;
	struct input_dev *input;
	int err;

	tcm = devm_kzalloc(&spi->dev, sizeof(*tcm), GFP_KERNEL);
	if (!tcm)
		return -ENOMEM;

	tcm->spi = spi;
	spi_set_drvdata(spi, tcm);
	mutex_init(&tcm->io_lock);

	tcm->tx = devm_kmalloc(&spi->dev, SYNA_TCM_MAX_FRAME, GFP_KERNEL);
	tcm->rx = devm_kmalloc(&spi->dev, SYNA_TCM_MAX_FRAME, GFP_KERNEL);
	if (!tcm->tx || !tcm->rx)
		return -ENOMEM;

	tcm->vdd = devm_regulator_get(&spi->dev, "vdd");
	if (IS_ERR(tcm->vdd))
		return dev_err_probe(&spi->dev, PTR_ERR(tcm->vdd),
				     "failed to get vdd\n");

	tcm->reset_gpio = devm_gpiod_get(&spi->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(tcm->reset_gpio))
		return dev_err_probe(&spi->dev, PTR_ERR(tcm->reset_gpio),
				     "failed to get reset GPIO\n");

	err = syna_tcm_power_on(tcm);
	if (err)
		return dev_err_probe(&spi->dev, err, "failed to power on\n");

	err = syna_tcm_identify(tcm);
	if (err)
		return dev_err_probe(&spi->dev, err, "identify failed\n");

	err = syna_tcm_get_app_info(tcm);
	if (err)
		return dev_err_probe(&spi->dev, err, "app info failed\n");

	err = syna_tcm_get_report_config(tcm);
	if (err)
		return dev_err_probe(&spi->dev, err,
				     "report config failed\n");

	input = devm_input_allocate_device(&spi->dev);
	if (!input)
		return -ENOMEM;

	input->name = "Synaptics S3910 Touchscreen";
	input->id.bustype = BUS_SPI;

	input_set_abs_params(input, ABS_MT_POSITION_X, 0, tcm->max_x, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0, tcm->max_y, 0, 0);
	input_set_abs_params(input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(input, ABS_MT_TOUCH_MINOR, 0, 255, 0, 0);
	input_set_abs_params(input, ABS_MT_PRESSURE, 0, 255, 0, 0);

	err = input_mt_init_slots(input, tcm->max_objects, INPUT_MT_DIRECT);
	if (err)
		return err;

	input_set_drvdata(input, tcm);
	tcm->input = input;

	err = input_register_device(input);
	if (err)
		return err;

	err = devm_request_threaded_irq(&spi->dev, spi->irq, NULL,
					syna_tcm_irq, IRQF_ONESHOT,
					"syna-tcm", tcm);
	if (err)
		return dev_err_probe(&spi->dev, err,
				     "failed to request IRQ\n");

	return 0;
}

static const struct of_device_id syna_tcm_of_match[] = {
	{ .compatible = "syna,s3910" },
	{ }
};
MODULE_DEVICE_TABLE(of, syna_tcm_of_match);

static const struct spi_device_id syna_tcm_spi_ids[] = {
	{ "syna-s3910" },
	{ }
};
MODULE_DEVICE_TABLE(spi, syna_tcm_spi_ids);

static struct spi_driver syna_tcm_spi_driver = {
	.driver = {
		.name = "synaptics_tcm_spi",
		.of_match_table = syna_tcm_of_match,
	},
	.probe = syna_tcm_probe,
	.id_table = syna_tcm_spi_ids,
};
module_spi_driver(syna_tcm_spi_driver);

MODULE_AUTHOR("Devajith Valaparambil Sreeramaswamy <devajithvs@gmail.com>");
MODULE_DESCRIPTION("Synaptics TouchComm v1 touchscreen driver over SPI");
MODULE_LICENSE("GPL");
