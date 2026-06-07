// SPDX-License-Identifier: GPL-2.0
/*
 * rpi-pwr-led.c - RPi4 firmware access via direct mailbox on UEFI/ACPI boot
 *
 * On Raspberry Pi 4 booted via UEFI+ACPI (e.g. Fedora CoreOS), the kernel
 * uses ACPI for device discovery and never populates the OF device tree, so
 * the standard raspberrypi-firmware MFD chain never initialises.
 *
 * This module bypasses that chain and drives the BCM2711 VideoCore mailbox
 * directly, providing:
 *
 *   /sys/class/leds/PWR/brightness    - red power LED (0=off, 1=on)
 *   /sys/bus/platform/devices/rpi-fw/throttled - firmware throttle bitmask
 *
 * Throttle bitmask (tag 0x00030046):
 *   bit  0  under-voltage currently detected
 *   bit  1  arm frequency currently capped
 *   bit  2  currently throttled
 *   bit  3  soft temperature limit currently active
 *   bit 16  under-voltage has occurred since last read
 *   bit 17  arm frequency has been capped since last read
 *   bit 18  throttling has occurred since last read
 *   bit 19  soft temperature limit has been reached since last read
 *
 * Key implementation notes (from reading gpio-raspberrypi-exp.c):
 *   - Expander GPIO namespace starts at 128 (RPI_EXP_GPIO_BASE); PWR LED is
 *     expgpio pin 2 = firmware GPIO 130.
 *   - SET_GPIO_CONFIG (0x00038043) must set direction=output before
 *     SET_GPIO_STATE (0x00038041) has any effect.
 *   - Mailbox buffer must be in the first 1 GB of ARM RAM (GFP_DMA).
 *   - ARM D-cache must be flushed explicitly (DC CIVAC) around each call.
 */

#include <linux/module.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/leds.h>
#include <linux/platform_device.h>
#include <linux/err.h>

/* ---- Cache flush ---- */

/* Clean and invalidate D-cache lines to PoC by virtual address.
 * Cortex-A72 (RPi4) has 64-byte cache lines. */
static void flush_buffer_poc(const void *buf, size_t len)
{
	unsigned long addr = (unsigned long)buf & ~63UL;
	unsigned long end  = (unsigned long)buf + len;

	for (; addr < end; addr += 64)
		asm volatile("dc civac, %0" :: "r"(addr) : "memory");
	asm volatile("dsb sy\nisb" ::: "memory");
}

/* ---- Mailbox registers ---- */

#define MBOX_BASE       0xfe00b880UL
#define MBOX_SIZE       0x40UL
#define MBOX0_READ      0x00		/* GPU -> ARM */
#define MBOX0_STATUS    0x18
#define MBOX0_RX_EMPTY  BIT(30)
#define MBOX1_WRITE     0x20		/* ARM -> GPU */
#define MBOX1_STATUS    0x38
#define MBOX1_TX_FULL   BIT(31)
#define PROP_CHAN       8U

/* First 1 GB of ARM RAM maps to VC bus 0xC0000000–0xFFFFFFFF */
#define VC_BUS_OFFSET   0xC0000000UL
#define ARM_LOW_MEM_MAX 0x40000000UL

/* ---- Firmware property tags ---- */

#define TAG_SET_GPIO_STATE  0x00038041U
#define TAG_GET_GPIO_CONFIG 0x00030043U
#define TAG_SET_GPIO_CONFIG 0x00038043U
#define TAG_GET_THROTTLED   0x00030046U

/* Expander GPIO base in firmware namespace (gpio-raspberrypi-exp.c) */
#define RPI_EXP_GPIO_BASE   128U
#define EXPGPIO_PWR         (2U + RPI_EXP_GPIO_BASE)

#define GPIO_DIR_OUT        1U

static void __iomem *mbox_virt;

/* ---- Generic mailbox property call ---- */

/* buf/len: value buffer — input on request, output on return. */
static int mbox_call(u32 tag, void *buf, size_t len)
{
	/* Full message: 2× header words + 3× tag header words + value + end */
	size_t msg_size = ALIGN(sizeof(u32) * 5 + len + sizeof(u32), 16);
	u32 *msg;
	phys_addr_t phys;
	u32 vc_addr, resp;
	int timeout, ret = 0;

	msg = kzalloc(msg_size, GFP_DMA | GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	phys = virt_to_phys(msg);
	if (phys >= ARM_LOW_MEM_MAX) {
		pr_err_once("rpi-fw: buffer at %pa above 1 GB\n", &phys);
		ret = -ENOMEM;
		goto out;
	}

	msg[0] = msg_size;	/* total size */
	msg[1] = 0;		/* request code */
	msg[2] = tag;
	msg[3] = (u32)len;	/* value buffer size */
	msg[4] = 0;		/* request indicator */
	memcpy(&msg[5], buf, len);
	msg[5 + ALIGN(len, 4) / 4] = 0; /* end tag */

	flush_buffer_poc(msg, msg_size);

	vc_addr = (u32)(phys + VC_BUS_OFFSET);

	timeout = 10000;
	while ((readl(mbox_virt + MBOX1_STATUS) & MBOX1_TX_FULL) && --timeout)
		udelay(10);
	if (!timeout) { ret = -ETIMEDOUT; goto out; }

	writel((vc_addr & ~0xfU) | PROP_CHAN, mbox_virt + MBOX1_WRITE);

	timeout = 100000;
	do {
		while ((readl(mbox_virt + MBOX0_STATUS) & MBOX0_RX_EMPTY) && --timeout)
			udelay(10);
		if (!timeout) { ret = -ETIMEDOUT; goto out; }
		resp = readl(mbox_virt + MBOX0_READ);
	} while ((resp & 0xf) != PROP_CHAN);

	flush_buffer_poc(msg, msg_size);

	if (msg[1] != 0x80000000U) {
		ret = -EIO;
		goto out;
	}

	memcpy(buf, &msg[5], len);
out:
	kfree(msg);
	return ret;
}

/* ---- GPIO ---- */

struct gpio_get_config {
	u32 gpio, direction, polarity, term_en, term_pull_up;
};

struct gpio_set_config {
	u32 gpio, direction, polarity, term_en, term_pull_up, state;
};

struct gpio_state {
	u32 gpio, state;
};

static int fw_gpio_set_output(u32 gpio, u32 state)
{
	struct gpio_get_config get = { .gpio = gpio };
	struct gpio_set_config set;
	int ret;

	ret = mbox_call(TAG_GET_GPIO_CONFIG, &get, sizeof(get));
	if (ret)
		return ret;

	set = (struct gpio_set_config){
		.gpio      = gpio,
		.direction = GPIO_DIR_OUT,
		.polarity  = get.polarity,
		.state     = state,
	};
	return mbox_call(TAG_SET_GPIO_CONFIG, &set, sizeof(set));
}

static int fw_gpio_set_state(u32 gpio, u32 state)
{
	struct gpio_state s = { .gpio = gpio, .state = state };

	return mbox_call(TAG_SET_GPIO_STATE, &s, sizeof(s));
}

/* ---- Throttle sysfs ---- */

static ssize_t throttled_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	u32 val[2] = { 0, 0 };
	int ret = mbox_call(TAG_GET_THROTTLED, val, sizeof(val));

	if (ret)
		return ret;
	return sysfs_emit(buf, "0x%08x\n", val[1]);
}
static DEVICE_ATTR_RO(throttled);

static struct attribute *rpi_fw_attrs[] = {
	&dev_attr_throttled.attr,
	NULL,
};
ATTRIBUTE_GROUPS(rpi_fw);

/* ---- PWR LED ---- */

static struct led_classdev pwr_led;

static void pwr_led_set(struct led_classdev *led, enum led_brightness value)
{
	/* active-low: brightness 1 (on) = GPIO low (state 0) */
	fw_gpio_set_state(EXPGPIO_PWR, value ? 0 : 1);
}

/* ---- Platform device ---- */

static struct platform_device *rpi_fw_pdev;

static int __init rpi_fw_init(void)
{
	int ret;

	mbox_virt = ioremap(MBOX_BASE, MBOX_SIZE);
	if (!mbox_virt)
		return -ENOMEM;

	/* Configure PWR LED as output and switch it off */
	ret = fw_gpio_set_output(EXPGPIO_PWR, 1);
	if (ret) {
		pr_err("rpi-fw: PWR LED config failed: %d\n", ret);
		iounmap(mbox_virt);
		return ret;
	}
	fw_gpio_set_state(EXPGPIO_PWR, 1);

	/* Register a platform device to host the throttled sysfs attribute */
	rpi_fw_pdev = platform_device_register_simple("rpi-fw", -1, NULL, 0);
	if (IS_ERR(rpi_fw_pdev)) {
		pr_warn("rpi-fw: platform device registration failed (%ld)\n",
			PTR_ERR(rpi_fw_pdev));
		rpi_fw_pdev = NULL;
	} else {
		ret = sysfs_create_groups(&rpi_fw_pdev->dev.kobj, rpi_fw_groups);
		if (ret)
			pr_warn("rpi-fw: sysfs group creation failed (%d)\n", ret);
	}

	/* Register PWR LED */
	pwr_led.name           = "PWR";
	pwr_led.brightness     = LED_OFF;
	pwr_led.max_brightness = 1;
	pwr_led.brightness_set = pwr_led_set;

	ret = led_classdev_register(NULL, &pwr_led);
	if (ret)
		pr_warn("rpi-fw: LED registration failed (%d)\n", ret);

	pr_info("rpi-fw: PWR LED off, throttle at /sys/bus/platform/devices/rpi-fw/throttled\n");
	return 0;
}

static void __exit rpi_fw_exit(void)
{
	led_classdev_unregister(&pwr_led);
	if (rpi_fw_pdev) {
		sysfs_remove_groups(&rpi_fw_pdev->dev.kobj, rpi_fw_groups);
		platform_device_unregister(rpi_fw_pdev);
	}
	iounmap(mbox_virt);
}

module_init(rpi_fw_init);
module_exit(rpi_fw_exit);

MODULE_AUTHOR("Steve Crawford");
MODULE_DESCRIPTION("RPi4 PWR LED and throttle status via VC mailbox (UEFI/ACPI boot)");
MODULE_LICENSE("GPL");
