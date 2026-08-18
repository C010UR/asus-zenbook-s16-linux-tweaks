// asus-camled.c - ASUS camera privacy LED driver
//
// The camera LED on this machine is controlled by the Embedded Controller.
// The DSDT exposes it via the ASUS WMI devid 0x00060078 (CAMERA_LED_NEG):
//   - set 0x02 -> LED on
//   - set 0x03 -> LED off
// A firmware flag (FSIS) blocks repeated changes per boot; it is reset by
// evaluating the EC0W(4) method before each change.  The EC "off" bit in
// register 0x50 must be cleared first or the DSDT's OFF path bails early.
//
// Exposes the LED as a standard LED class device so userspace (camled.py)
// can mirror the webcam privacy control onto it.
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/leds.h>
#include <linux/device.h>
#include <linux/acpi.h>
#include <linux/platform_data/x86/asus-wmi.h>

#define ASUS_CAMLED_EC0W_PATH "\\_SB.PCI0.SBRG.EC0.EC0W"
#define ASUS_CAMLED_ST9E_PATH "\\_SB.PCI0.SBRG.EC0.ST9E"

static struct led_classdev camled_led;

static int camled_clear_off(void)
{
	struct acpi_object_list arg;
	union acpi_object param[3];
	acpi_status status;

	arg.count = 3;
	arg.pointer = param;
	param[0].type = ACPI_TYPE_INTEGER;
	param[0].integer.value = 0x50;
	param[1].type = ACPI_TYPE_INTEGER;
	param[1].integer.value = 0x02;
	param[2].type = ACPI_TYPE_INTEGER;
	param[2].integer.value = 0x00;

	status = acpi_evaluate_object(NULL, ASUS_CAMLED_ST9E_PATH, &arg, NULL);
	return ACPI_FAILURE(status) ? -EIO : 0;
}

static int camled_reset_fsis(void)
{
	struct acpi_object_list arg;
	union acpi_object param[1];
	acpi_status status;

	arg.count = 1;
	arg.pointer = param;
	param[0].type = ACPI_TYPE_INTEGER;
	param[0].integer.value = 4;

	status = acpi_evaluate_object(NULL, ASUS_CAMLED_EC0W_PATH, &arg, NULL);
	return ACPI_FAILURE(status) ? -EIO : 0;
}

static int camled_set(struct led_classdev *led_cdev,
		      enum led_brightness brightness)
{
	int state = brightness != LED_OFF;

	camled_clear_off();
	camled_reset_fsis();
	return asus_wmi_set_devstate(ASUS_WMI_DEVID_CAMERA_LED_NEG,
				     state ? 0x02 : 0x03, NULL);
}

static ssize_t toggle_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	int state = camled_led.brightness != LED_OFF;
	camled_set(&camled_led, state ? 0 : 1);
	return count;
}

static DEVICE_ATTR_WO(toggle);

static struct attribute *camled_attrs[] = {
	&dev_attr_toggle.attr,
	NULL,
};

static struct attribute_group camled_group = {
	.attrs = camled_attrs,
};

static int __init asus_camled_init(void)
{
	int err;

	camled_led.name = "asus::camera";
	camled_led.max_brightness = 1;
	camled_led.brightness_set_blocking = camled_set;

	err = led_classdev_register(NULL, &camled_led);
	if (err)
		return err;

	err = sysfs_create_group(&camled_led.dev->kobj, &camled_group);
	if (err)
		pr_err("asus-camled: sysfs group failed\n");

	pr_info("asus-camled: loaded\n");
	return 0;
}

static void __exit asus_camled_exit(void)
{
	sysfs_remove_group(&camled_led.dev->kobj, &camled_group);
	led_classdev_unregister(&camled_led);
}

module_init(asus_camled_init);
module_exit(asus_camled_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ASUS camera privacy LED driver");