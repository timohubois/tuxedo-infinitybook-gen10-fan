// SPDX-License-Identifier: GPL-2.0+
/*
 * TUXEDO InfinityBook Gen10 Fan Control
 * - Hwmon driver exposing manual PWM control for TUXEDO InfinityBook Pro AMD Gen10
 * - Coexists with upstream uniwill-laptop (Linux 6.19+) by using a separate hwmon
 *   device name ("uniwill_ibg10_fanctl")
 * - Provides temperature readout and manual PWM control (0-255 hwmon scale)
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/wmi.h>

MODULE_DESCRIPTION("Fan control for TUXEDO InfinityBook Pro AMD Gen10");
MODULE_AUTHOR("Timo Hubois");
MODULE_VERSION("0.2.1");
MODULE_LICENSE("GPL");

/* WMI GUIDs for Uniwill laptops */
#define UNIWILL_WMI_MGMT_GUID_BC "ABBC0F6F-8EA1-11D1-00A0-C90629100000"
MODULE_ALIAS("wmi:" UNIWILL_WMI_MGMT_GUID_BC);

/* EC addresses for custom fan table control */
#define UW_EC_REG_USE_CUSTOM_FAN_TABLE_0    0x07c5
#define UW_EC_REG_USE_CUSTOM_FAN_TABLE_1    0x07c6

#define UW_EC_REG_CPU_FAN_TABLE_END_TEMP    0x0f00
#define UW_EC_REG_CPU_FAN_TABLE_START_TEMP  0x0f10
#define UW_EC_REG_CPU_FAN_TABLE_FAN_SPEED   0x0f20

#define UW_EC_REG_GPU_FAN_TABLE_END_TEMP    0x0f30
#define UW_EC_REG_GPU_FAN_TABLE_START_TEMP  0x0f40
#define UW_EC_REG_GPU_FAN_TABLE_FAN_SPEED   0x0f50

/* Direct fan control registers */
#define UW_EC_REG_FAN1_SPEED   0x1804
#define UW_EC_REG_FAN2_SPEED   0x1809

/* Temperature sensor */
#define UW_EC_REG_FAN1_TEMP    0x043e  /* CPU temp */

/* Fan mode register */
#define UW_EC_REG_FAN_MODE     0x0751
#define UW_EC_FAN_MODE_BIT     0x40

/* Manual mode control */
#define UW_EC_REG_MANUAL_MODE  0x0741

/* Custom profile mode */
#define UW_EC_REG_CUSTOM_PROFILE 0x0727
#define UW_EC_CUSTOM_PROFILE_BIT 0x40  /* bit 6 */

#define FAN_SPEED_MAX          200  /* EC scale */
#define FAN_ON_MIN_SPEED       25   /* ~12.5% to avoid EC fighting */

struct ibg10_data {
	struct platform_device *pdev;
	struct device *hwmon_dev;
	bool fans_initialized;
	struct mutex ec_lock;
};

/*
 * The EC signals a failed access by filling the return buffer with 0xfe
 * (tuxedo-drivers checks the first dword against 0xfefefefe). Such errors
 * happen transiently when the EC is busy, so retry a few times. Treating
 * the marker as data corrupts the EC on read-modify-write cycles.
 */
#define UW_EC_RETRIES		5
#define UW_EC_ERROR_MARKER	0xfefefefe

static int uw_ec_read(struct ibg10_data *data, u16 addr, u8 *value)
{
	acpi_status status;
	union acpi_object *out_obj;
	u32 wmi_arg[10];
	u8 *arg_bytes = (u8 *)wmi_arg;
	struct acpi_buffer in = { sizeof(wmi_arg), wmi_arg };
	struct acpi_buffer out;
	int retries = UW_EC_RETRIES;
	u32 result_dword;
	int ret;

	mutex_lock(&data->ec_lock);

retry:
	out = (struct acpi_buffer){ ACPI_ALLOCATE_BUFFER, NULL };
	ret = 0;
	memset(wmi_arg, 0, sizeof(wmi_arg));

	arg_bytes[0] = addr & 0xff;
	arg_bytes[1] = (addr >> 8) & 0xff;
	arg_bytes[5] = 1; /* read */

	status = wmi_evaluate_method(UNIWILL_WMI_MGMT_GUID_BC, 0, 4, &in, &out);
	out_obj = out.pointer;
	if (ACPI_FAILURE(status)) {
		ret = -EIO;
	} else if (out_obj && out_obj->type == ACPI_TYPE_BUFFER && out_obj->buffer.length >= 4) {
		memcpy(&result_dword, out_obj->buffer.pointer, 4);
		if (result_dword == UW_EC_ERROR_MARKER)
			ret = -EIO;
		else
			*value = out_obj->buffer.pointer[0];
	} else {
		ret = -EIO;
	}

	kfree(out.pointer);

	if (ret && --retries > 0) {
		msleep(20);
		goto retry;
	}
	if (ret)
		pr_err("WMI read failed for addr 0x%04x\n", addr);

	mutex_unlock(&data->ec_lock);
	return ret;
}

static int uw_ec_write(struct ibg10_data *data, u16 addr, u8 value)
{
	acpi_status status;
	union acpi_object *out_obj;
	u32 wmi_arg[10];
	u8 *arg_bytes = (u8 *)wmi_arg;
	struct acpi_buffer in = { sizeof(wmi_arg), wmi_arg };
	struct acpi_buffer out;
	int retries = UW_EC_RETRIES;
	u32 result_dword;
	int ret;

	mutex_lock(&data->ec_lock);

retry:
	out = (struct acpi_buffer){ ACPI_ALLOCATE_BUFFER, NULL };
	ret = 0;
	memset(wmi_arg, 0, sizeof(wmi_arg));

	arg_bytes[0] = addr & 0xff;
	arg_bytes[1] = (addr >> 8) & 0xff;
	arg_bytes[2] = value;
	arg_bytes[5] = 0; /* write */

	status = wmi_evaluate_method(UNIWILL_WMI_MGMT_GUID_BC, 0, 4, &in, &out);
	out_obj = out.pointer;
	if (ACPI_FAILURE(status)) {
		ret = -EIO;
	} else if (out_obj && out_obj->type == ACPI_TYPE_BUFFER && out_obj->buffer.length >= 4) {
		memcpy(&result_dword, out_obj->buffer.pointer, 4);
		if (result_dword == UW_EC_ERROR_MARKER)
			ret = -EIO;
	}

	kfree(out.pointer);

	if (ret && --retries > 0) {
		msleep(20);
		goto retry;
	}
	if (ret)
		pr_err("WMI write failed for addr 0x%04x\n", addr);

	mutex_unlock(&data->ec_lock);
	return ret;
}

static int init_custom_fan_table(struct ibg10_data *data)
{
	u8 val0, val1;
	int i;
	int ret;
	u8 temp_offset = 115;

	if (data->fans_initialized)
		return 0;

	pr_info("Initializing custom fan table...\n");

	/* Toggle custom profile bit */
	ret = uw_ec_read(data, UW_EC_REG_CUSTOM_PROFILE, &val0);
	if (ret < 0)
		return ret;
	val0 &= ~UW_EC_CUSTOM_PROFILE_BIT;
	uw_ec_write(data, UW_EC_REG_CUSTOM_PROFILE, val0);
	msleep(50);
	val0 |= UW_EC_CUSTOM_PROFILE_BIT;
	uw_ec_write(data, UW_EC_REG_CUSTOM_PROFILE, val0);

	/* Enable manual mode */
	uw_ec_write(data, UW_EC_REG_MANUAL_MODE, 0x01);

	/* Disable full fan mode */
	ret = uw_ec_read(data, UW_EC_REG_FAN_MODE, &val0);
	if (ret < 0)
		return ret;
	if (val0 & UW_EC_FAN_MODE_BIT)
		uw_ec_write(data, UW_EC_REG_FAN_MODE, val0 & ~UW_EC_FAN_MODE_BIT);

	/* Enable custom fan table 0 (bit 7) */
	ret = uw_ec_read(data, UW_EC_REG_USE_CUSTOM_FAN_TABLE_0, &val0);
	if (ret < 0)
		return ret;
	if (!((val0 >> 7) & 1))
		uw_ec_write(data, UW_EC_REG_USE_CUSTOM_FAN_TABLE_0, val0 | BIT(7));

	/* CPU fan table - zone 0 */
	uw_ec_write(data, UW_EC_REG_CPU_FAN_TABLE_END_TEMP, 115);
	uw_ec_write(data, UW_EC_REG_CPU_FAN_TABLE_START_TEMP, 0);
	uw_ec_write(data, UW_EC_REG_CPU_FAN_TABLE_FAN_SPEED, 0x00);

	/* GPU fan table - zone 0 */
	uw_ec_write(data, UW_EC_REG_GPU_FAN_TABLE_END_TEMP, 120);
	uw_ec_write(data, UW_EC_REG_GPU_FAN_TABLE_START_TEMP, 0);
	uw_ec_write(data, UW_EC_REG_GPU_FAN_TABLE_FAN_SPEED, 0x00);

	for (i = 1; i <= 15; i++) {
		uw_ec_write(data, UW_EC_REG_CPU_FAN_TABLE_END_TEMP + i, temp_offset + i + 1);
		uw_ec_write(data, UW_EC_REG_CPU_FAN_TABLE_START_TEMP + i, temp_offset + i);
		uw_ec_write(data, UW_EC_REG_CPU_FAN_TABLE_FAN_SPEED + i, FAN_SPEED_MAX);

		uw_ec_write(data, UW_EC_REG_GPU_FAN_TABLE_END_TEMP + i, temp_offset + i + 1);
		uw_ec_write(data, UW_EC_REG_GPU_FAN_TABLE_START_TEMP + i, temp_offset + i);
		uw_ec_write(data, UW_EC_REG_GPU_FAN_TABLE_FAN_SPEED + i, FAN_SPEED_MAX);
	}

	/* Enable custom fan table 1 (bit 2) */
	ret = uw_ec_read(data, UW_EC_REG_USE_CUSTOM_FAN_TABLE_1, &val1);
	if (ret < 0)
		return ret;
	if (!((val1 >> 2) & 1))
		uw_ec_write(data, UW_EC_REG_USE_CUSTOM_FAN_TABLE_1, val1 | BIT(2));

	data->fans_initialized = true;
	pr_info("Custom fan table initialized\n");
	return 0;
}

static int fan_get_temp(struct ibg10_data *data)
{
	u8 temp;

	if (uw_ec_read(data, UW_EC_REG_FAN1_TEMP, &temp) < 0)
		return -EIO;

	return temp * 1000; /* millidegree C */
}

static int fan_get_speed(struct ibg10_data *data, int fan_idx)
{
	u8 speed;
	u16 addr = (fan_idx == 0) ? UW_EC_REG_FAN1_SPEED : UW_EC_REG_FAN2_SPEED;

	if (uw_ec_read(data, addr, &speed) < 0)
		return -EIO;

	return speed;
}

static int fan_set_speed(struct ibg10_data *data, int fan_idx, u8 speed)
{
	u16 table_addr, direct_addr;
	int i;

	if (!data->fans_initialized)
		init_custom_fan_table(data);

	table_addr = (fan_idx == 0) ? UW_EC_REG_CPU_FAN_TABLE_FAN_SPEED : UW_EC_REG_GPU_FAN_TABLE_FAN_SPEED;
	direct_addr = (fan_idx == 0) ? UW_EC_REG_FAN1_SPEED : UW_EC_REG_FAN2_SPEED;

	if (speed > FAN_SPEED_MAX)
		speed = FAN_SPEED_MAX;

	if (speed == 0)
		speed = 1;
	else if (speed < FAN_ON_MIN_SPEED)
		speed = FAN_ON_MIN_SPEED;

	uw_ec_write(data, table_addr, speed);

	for (i = 0; i < 5; i++) {
		uw_ec_write(data, direct_addr, speed);
		msleep(10);
	}

	return 0;
}

static int fan_set_auto(struct ibg10_data *data)
{
	u8 val0, val1, mode;

	/*
	 * Called from module exit, so restore best-effort: a failed read only
	 * skips its own read-modify-write instead of aborting the sequence.
	 */
	if (!uw_ec_read(data, UW_EC_REG_USE_CUSTOM_FAN_TABLE_1, &val1) &&
	    (val1 >> 2) & 1)
		uw_ec_write(data, UW_EC_REG_USE_CUSTOM_FAN_TABLE_1, val1 & ~BIT(2));

	if (!uw_ec_read(data, UW_EC_REG_USE_CUSTOM_FAN_TABLE_0, &val0) &&
	    (val0 >> 7) & 1)
		uw_ec_write(data, UW_EC_REG_USE_CUSTOM_FAN_TABLE_0, val0 & ~BIT(7));

	if (!uw_ec_read(data, UW_EC_REG_FAN_MODE, &mode) &&
	    (mode & UW_EC_FAN_MODE_BIT))
		uw_ec_write(data, UW_EC_REG_FAN_MODE, mode & ~UW_EC_FAN_MODE_BIT);

	uw_ec_write(data, UW_EC_REG_MANUAL_MODE, 0x00);

	if (!uw_ec_read(data, UW_EC_REG_CUSTOM_PROFILE, &val0) &&
	    (val0 & UW_EC_CUSTOM_PROFILE_BIT))
		uw_ec_write(data, UW_EC_REG_CUSTOM_PROFILE, val0 & ~UW_EC_CUSTOM_PROFILE_BIT);

	data->fans_initialized = false;
	pr_info("Restored automatic fan control\n");
	return 0;
}

/* hwmon callbacks */
static umode_t ibg10_is_visible(const void *drvdata, enum hwmon_sensor_types type,
				u32 attr, int channel)
{
	switch (type) {
	case hwmon_temp:
		if (attr == hwmon_temp_input || attr == hwmon_temp_label)
			return 0444;
		break;
	case hwmon_pwm:
		if (attr == hwmon_pwm_input || attr == hwmon_pwm_enable)
			return 0644;
		break;
	default:
		break;
	}

	return 0;
}

static int ibg10_read(struct device *dev, enum hwmon_sensor_types type,
			u32 attr, int channel, long *val)
{
	struct ibg10_data *data = dev_get_drvdata(dev);
	int ret;

	switch (type) {
	case hwmon_temp:
		if (attr == hwmon_temp_input) {
			ret = fan_get_temp(data);
			if (ret < 0)
				return ret;
			*val = ret;
			return 0;
		}
		break;
	case hwmon_pwm:
		if (attr == hwmon_pwm_input) {
			ret = fan_get_speed(data, channel);
			if (ret < 0)
				return ret;
			*val = (ret * 255) / FAN_SPEED_MAX;
			return 0;
		} else if (attr == hwmon_pwm_enable) {
			*val = data->fans_initialized ? 1 : 2;
			return 0;
		}
		break;
	default:
		break;
	}

	return -EOPNOTSUPP;
}

static int ibg10_read_string(struct device *dev, enum hwmon_sensor_types type,
				u32 attr, int channel, const char **str)
{
	switch (type) {
	case hwmon_temp:
		if (attr == hwmon_temp_label) {
			*str = "CPU";
			return 0;
		}
		break;
	default:
		break;
	}

	return -EOPNOTSUPP;
}

static int ibg10_write(struct device *dev, enum hwmon_sensor_types type,
			u32 attr, int channel, long val)
{
	struct ibg10_data *data = dev_get_drvdata(dev);
	u8 speed;

	switch (type) {
	case hwmon_pwm:
		if (attr == hwmon_pwm_input) {
			if (val < 0 || val > 255)
				return -EINVAL;
			speed = (val * FAN_SPEED_MAX) / 255;
			return fan_set_speed(data, channel, speed);
		} else if (attr == hwmon_pwm_enable) {
			if (val == 2)
				return fan_set_auto(data);
			else if (val == 1)
				return init_custom_fan_table(data);
			return -EINVAL;
		}
		break;
	default:
		break;
	}

	return -EOPNOTSUPP;
}

static const struct hwmon_ops ibg10_hwmon_ops = {
	.is_visible = ibg10_is_visible,
	.read = ibg10_read,
	.read_string = ibg10_read_string,
	.write = ibg10_write,
};

static const struct hwmon_channel_info *const ibg10_info[] = {
	HWMON_CHANNEL_INFO(chip, HWMON_C_REGISTER_TZ),
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(pwm,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE),
	NULL
};

static const struct hwmon_chip_info ibg10_chip_info = {
	.ops = &ibg10_hwmon_ops,
	.info = ibg10_info,
};

static struct ibg10_data *gdata;

static int __init ibg10_fan_init(void)
{
	int ret;

	if (!wmi_has_guid(UNIWILL_WMI_MGMT_GUID_BC)) {
		pr_err("Uniwill WMI GUID not found\n");
		return -ENODEV;
	}

	gdata = kzalloc(sizeof(*gdata), GFP_KERNEL);
	if (!gdata)
		return -ENOMEM;

	mutex_init(&gdata->ec_lock);

	gdata->pdev = platform_device_register_simple("tuxedo_ibg10_fan", -1, NULL, 0);
	if (IS_ERR(gdata->pdev)) {
		ret = PTR_ERR(gdata->pdev);
		kfree(gdata);
		gdata = NULL;
		return ret;
	}

	platform_set_drvdata(gdata->pdev, gdata);

	gdata->hwmon_dev = devm_hwmon_device_register_with_info(&gdata->pdev->dev,
			     "uniwill_ibg10_fanctl", gdata,
			     &ibg10_chip_info, NULL);

	if (IS_ERR(gdata->hwmon_dev)) {
		ret = PTR_ERR(gdata->hwmon_dev);
		platform_device_unregister(gdata->pdev);
		kfree(gdata);
		gdata = NULL;
		return ret;
	}

	pr_info("Registered hwmon device 'uniwill_ibg10_fanctl'\n");
	return 0;
}

static void __exit ibg10_fan_exit(void)
{
	if (!gdata)
		return;

	fan_set_auto(gdata);
	platform_device_unregister(gdata->pdev);
	kfree(gdata);
	gdata = NULL;
}

module_init(ibg10_fan_init);
module_exit(ibg10_fan_exit);

