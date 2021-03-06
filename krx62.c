// SPDX-License-Identifier: GPL-2.0+
/*
 * krx62.c - hwmon driver for NZXT Kraken X42/X52/X62/X72
 *
 * Copyright 2019  Jonas Malaco <jonas@protocubo.io>
 */

#include <linux/hid.h>
#include <linux/hwmon.h>
#include <linux/kernel.h>
#include <linux/module.h>

#define KRX62_RPM_INPUTS		2

struct krx62_device_data {
	struct hid_device *hid_dev;
	struct device *hwmon_dev;

	long temp_input;
	long fan_input[KRX62_RPM_INPUTS];
};

static umode_t krx62_is_visible(const void *data,
				enum hwmon_sensor_types type,
				u32 attr, int channel)
{
	return 0444;
}

static int krx62_read(struct device *dev, enum hwmon_sensor_types type,
		      u32 attr, int channel, long *val)
{
	struct krx62_device_data *ldata = dev_get_drvdata(dev);

	switch (type) {
	case hwmon_temp:
		*val = ldata->temp_input;
		break;
	case hwmon_fan:
		*val = ldata->fan_input[channel];
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

#define KRX62_TEMP_LABEL		"Coolant"

static const char *const krx62_fan_label[] = {
	"Fans",
	"Pump",
};

static int krx62_read_string(struct device *dev,
			     enum hwmon_sensor_types type, u32 attr,
			     int channel, const char **str)
{
	switch (type) {
	case hwmon_temp:
		*str = KRX62_TEMP_LABEL;
		break;
	case hwmon_fan:
		*str = krx62_fan_label[channel];
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static const struct hwmon_ops krx62_hwmon_ops = {
	.is_visible = krx62_is_visible,
	.read = krx62_read,
	.read_string = krx62_read_string,
};

#define DEVNAME_KRAKEN_GEN3	"krakenx"  /* FIXME */

static const struct hwmon_channel_info *krx62_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT | HWMON_F_LABEL, HWMON_F_INPUT | HWMON_F_LABEL),
	NULL			/* TODO pwm */
};

static const struct hwmon_chip_info krx62_chip_info = {
	.ops = &krx62_hwmon_ops,
	.info = krx62_info,
};

#define USB_VENDOR_ID_NZXT		0x1e71
#define USB_DEVICE_ID_KRAKEN_GEN3	0x170e

#define STATUS_REPORT_ID		4
#define STATUS_MIN_BYTES		16

#define show_ctx() \
	printk(KERN_DEBUG "%s:%d: irq: %lu, serving_softirq: %lu, nmi: %lu, task: %u\n", \
	       __FUNCTION__, __LINE__, in_irq(), in_serving_softirq(), in_nmi(), in_task());

static int krx62_raw_event(struct hid_device *hdev,
			       struct hid_report *report, u8 *data, int size)
{
	struct krx62_device_data *ldata;

	/* TODO we're in a hard irq, how much should we do here? */

	/* TODO we only want one report, specify it in hid_driver */
	if (report->id != STATUS_REPORT_ID || size < STATUS_MIN_BYTES)
		return 0;

	ldata = hid_get_drvdata(hdev);

	/* FIXME missing locking */
	ldata->temp_input = data[1] * 1000 + data[2] * 100;
	ldata->fan_input[0] = be16_to_cpup((__be16 *) (data + 3));
	ldata->fan_input[1] = be16_to_cpup((__be16 *) (data + 5));
	return 0;
}

static const struct hid_device_id krx62_table[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_NZXT, USB_DEVICE_ID_KRAKEN_GEN3) },
	{ }
};

MODULE_DEVICE_TABLE(hid, krx62_table);

static int krx62_probe(struct hid_device *hdev,
		       const struct hid_device_id *id)
{
	struct krx62_device_data *ldata;
	struct device *hwmon_dev;
	const struct hwmon_chip_info *chip_info;
	char *chip_name;
	int ret;

	ldata = devm_kzalloc(&hdev->dev, sizeof(*ldata), GFP_KERNEL);
	if (!ldata)
		return -ENOMEM;

	chip_name = DEVNAME_KRAKEN_GEN3;
	chip_info = &krx62_chip_info;
	hid_info(hdev, "device: %s\n", chip_name);

	ldata->hid_dev = hdev;
	hid_set_drvdata(hdev, ldata);

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "hid_parse failed with %d\n", ret);
		return ret;
	}

	/* keep hidraw so user-space can (easily) take care of the other
	 * features of the device (e.g. LEDs) */
	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret) {
		hid_err(hdev, "hid_hw_start failed with %d\n", ret);
		goto rec_stop_hid;
	}

	ret = hid_hw_open(hdev);
	if (ret) {
		hid_err(hdev, "hid_hw_open failed with %d\n", ret);
		goto rec_close_hid;
	}

	hwmon_dev = devm_hwmon_device_register_with_info(&hdev->dev, chip_name,
							 ldata, chip_info,
							 NULL);
	if (IS_ERR(hwmon_dev)) {
		hid_err(hdev, "failed to register hwmon device\n");
		ret = PTR_ERR(hwmon_dev);
		goto rec_close_hid;
	}
	ldata->hwmon_dev = hwmon_dev;

	hid_info(hdev, "probing successful\n");
	return 0;

rec_close_hid:
	hid_hw_close(hdev);
rec_stop_hid:
	hid_hw_stop(hdev);
	return ret;
}

static void krx62_remove(struct hid_device *hdev)
{
	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static struct hid_driver krx62_driver = {
	.name = "krx62",
	.id_table = krx62_table,
	.probe = krx62_probe,
	.remove = krx62_remove,
	.raw_event = krx62_raw_event,
};

module_hid_driver(krx62_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jonas Malaco <jonas@protocubo.io>");
MODULE_DESCRIPTION("Hwmon driver for NZXT Kraken X42/X52/X62/X72");
