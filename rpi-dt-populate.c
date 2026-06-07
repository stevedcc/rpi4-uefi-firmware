// SPDX-License-Identifier: GPL-2.0
/*
 * rpi-dt-populate.c - RPi4 firmware access via direct mailbox on UEFI/ACPI boot
 *
 * On Raspberry Pi 4 booted via UEFI+ACPI (e.g. Fedora CoreOS), the kernel
 * uses ACPI for device discovery and never populates the OF device tree.
 * The usual driver chain (bcm2835-mbox -> raspberrypi-firmware -> firmware-gpio
 * / raspberrypi-hwmon) never initialises, leaving the SoC temperature sensor
 * and PWR LED inaccessible.
 *
 * This module drives the BCM2711 VideoCore mailbox directly via the property
 * interface (channel 8) and exposes:
 *   /sys/class/hwmon/hwmonN/temp1_input  - SoC temperature (millidegrees C)
 *   /sys/class/leds/PWR/                 - PWR LED control (sysfs)
 *
 * Key finding from gpio-raspberrypi-exp.c: expander GPIOs are numbered
 * starting at RPI_EXP_GPIO_BASE (128) in the firmware namespace. Pin 2 of
 * expgpio (PWR LED) = firmware GPIO 130. Direction must be explicitly set
 * to output via SET_GPIO_CONFIG before SET_GPIO_STATE has any effect.
 */

#include <linux/module.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/leds.h>
#include <linux/hwmon.h>
#include <linux/err.h>

/* DC CIVAC: clean+invalidate D-cache to PoC. Cortex-A72 cache line = 64 B. */
static void flush_buffer_poc(const void *buf, size_t len)
{
    unsigned long addr = (unsigned long)buf & ~63UL;
    unsigned long end  = (unsigned long)buf + len;

    for (; addr < end; addr += 64)
        asm volatile("dc civac, %0" :: "r"(addr) : "memory");
    asm volatile("dsb sy\nisb" ::: "memory");
}

/* BCM2711 mailbox register map (base 0xfe00b880) */
#define MBOX_BASE       0xfe00b880UL
#define MBOX_SIZE       0x40UL
#define MBOX0_READ      0x00
#define MBOX0_STATUS    0x18
#define MBOX0_RX_EMPTY  BIT(30)
#define MBOX1_WRITE     0x20
#define MBOX1_STATUS    0x38
#define MBOX1_TX_FULL   BIT(31)
#define PROP_CHAN       8U
#define VC_BUS_OFFSET   0xC0000000UL
#define ARM_LOW_MEM_MAX 0x40000000UL

/* Firmware property tags */
#define TAG_GET_TEMPERATURE     0x00030006U
#define TAG_GET_GPIO_STATE      0x00030041U
#define TAG_SET_GPIO_STATE      0x00038041U
#define TAG_GET_GPIO_CONFIG     0x00030043U
#define TAG_SET_GPIO_CONFIG     0x00038043U

/*
 * Expander GPIO base in the firmware namespace (from gpio-raspberrypi-exp.c).
 * expgpio pin N = firmware GPIO (N + RPI_EXP_GPIO_BASE).
 */
#define RPI_EXP_GPIO_BASE   128U
#define EXPGPIO_PWR         (2U + RPI_EXP_GPIO_BASE)   /* = 130 */

#define GPIO_DIR_OUT        1U

static void __iomem *mbox_virt;

/*
 * Generic mailbox property call.
 * buf/len: the value buffer for the tag (input on call, output on return).
 */
static int mbox_call(u32 tag, void *buf, size_t len)
{
    /* Full property message: header + tag header + value buf + end tag */
    size_t msg_size = 4 * sizeof(u32) + ALIGN(len, 4) + sizeof(u32);
    u32 *msg;
    phys_addr_t phys;
    u32 vc_addr, resp;
    int timeout, ret = 0;
    u32 *tag_header;

    msg_size = ALIGN(msg_size, 16);
    msg = kzalloc(msg_size, GFP_DMA | GFP_KERNEL);
    if (!msg)
        return -ENOMEM;

    phys = virt_to_phys(msg);
    if (phys >= ARM_LOW_MEM_MAX) {
        pr_err_once("rpi-fw: buffer at %pa above 1 GB\n", &phys);
        ret = -ENOMEM;
        goto out;
    }

    /* Property message header */
    msg[0] = msg_size;          /* total size */
    msg[1] = 0;                 /* request code */

    /* Tag */
    tag_header = &msg[2];
    tag_header[0] = tag;
    tag_header[1] = (u32)len;   /* value buffer size */
    tag_header[2] = 0;          /* request indicator */
    memcpy(&tag_header[3], buf, len);

    /* End tag */
    msg[2 + 3 + ALIGN(len, 4) / 4] = 0;

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
        pr_debug("rpi-fw: tag 0x%08x firmware error 0x%08x\n", tag, msg[1]);
        ret = -EIO;
        goto out;
    }

    memcpy(buf, &tag_header[3], len);
out:
    kfree(msg);
    return ret;
}

/* ---- GPIO helpers ---- */

struct gpio_get_config {
    u32 gpio;
    u32 direction;
    u32 polarity;
    u32 term_en;
    u32 term_pull_up;
};

struct gpio_set_config {
    u32 gpio;
    u32 direction;
    u32 polarity;
    u32 term_en;
    u32 term_pull_up;
    u32 state;
};

struct gpio_state {
    u32 gpio;
    u32 state;
};

static int fw_gpio_set_output(u32 gpio, u32 state)
{
    struct gpio_get_config get = { .gpio = gpio };
    struct gpio_set_config set;
    int ret;

    /* Read current config to preserve polarity */
    ret = mbox_call(TAG_GET_GPIO_CONFIG, &get, sizeof(get));
    if (ret) {
        pr_warn("rpi-fw: GET_GPIO_CONFIG gpio=%u failed: %d\n", gpio, ret);
        return ret;
    }

    set.gpio        = gpio;
    set.direction   = GPIO_DIR_OUT;
    set.polarity    = get.polarity;
    set.term_en     = 0;
    set.term_pull_up = 0;
    set.state       = state;

    ret = mbox_call(TAG_SET_GPIO_CONFIG, &set, sizeof(set));
    if (ret)
        pr_warn("rpi-fw: SET_GPIO_CONFIG gpio=%u failed: %d\n", gpio, ret);
    return ret;
}

static int fw_gpio_set_state(u32 gpio, u32 state)
{
    struct gpio_state s = { .gpio = gpio, .state = state };
    return mbox_call(TAG_SET_GPIO_STATE, &s, sizeof(s));
}

/* ---- hwmon ---- */

static umode_t rpi_hwmon_is_visible(const void *d,
                                    enum hwmon_sensor_types t,
                                    u32 attr, int ch)
{
    return 0444;
}

static int rpi_hwmon_read(struct device *dev, enum hwmon_sensor_types t,
                          u32 attr, int ch, long *val)
{
    u32 buf[2] = { 0, 0 };     /* temp_id=0, out: millidegrees C */
    int ret = mbox_call(TAG_GET_TEMPERATURE, buf, sizeof(buf));

    if (ret)
        return ret;
    *val = buf[1];
    return 0;
}

static const struct hwmon_channel_info * const rpi_hwmon_info[] = {
    HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT),
    NULL
};

static const struct hwmon_ops rpi_hwmon_ops = {
    .is_visible = rpi_hwmon_is_visible,
    .read       = rpi_hwmon_read,
};

static const struct hwmon_chip_info rpi_chip_info = {
    .ops  = &rpi_hwmon_ops,
    .info = rpi_hwmon_info,
};

/* ---- LED ---- */

static struct led_classdev pwr_led;

static void pwr_led_set(struct led_classdev *led, enum led_brightness value)
{
    /*
     * active-low LED: brightness=1 (on) -> GPIO low (state=0)
     *                 brightness=0 (off) -> GPIO high (state=1)
     */
    fw_gpio_set_state(EXPGPIO_PWR, value ? 0 : 1);
}

/* ---- module init / exit ---- */

static struct device *hwmon_dev;

static int __init rpi_fw_init(void)
{
    u32 temp_buf[2] = { 0, 0 };
    int ret;

    mbox_virt = ioremap(MBOX_BASE, MBOX_SIZE);
    if (!mbox_virt)
        return -ENOMEM;

    /* Sanity-check: read temperature */
    ret = mbox_call(TAG_GET_TEMPERATURE, temp_buf, sizeof(temp_buf));
    if (ret) {
        pr_err("rpi-fw: mailbox not responding: %d\n", ret);
        iounmap(mbox_virt);
        return ret;
    }
    pr_info("rpi-fw: SoC temp %u.%u C\n",
            temp_buf[1] / 1000, (temp_buf[1] % 1000) / 100);

    /* hwmon */
    hwmon_dev = hwmon_device_register_with_info(NULL, "rpi_fw", NULL,
                                                &rpi_chip_info, NULL);
    if (IS_ERR(hwmon_dev)) {
        pr_warn("rpi-fw: hwmon registration failed (%ld)\n",
                PTR_ERR(hwmon_dev));
        hwmon_dev = NULL;
    }

    /* Configure PWR LED GPIO as output, then drive high (LED off, active-low) */
    ret = fw_gpio_set_output(EXPGPIO_PWR, 1);
    if (ret)
        pr_warn("rpi-fw: PWR LED config failed: %d\n", ret);
    else
        fw_gpio_set_state(EXPGPIO_PWR, 1);

    /* Register LED class device */
    pwr_led.name           = "PWR";
    pwr_led.brightness     = LED_OFF;
    pwr_led.max_brightness = 1;
    pwr_led.brightness_set = pwr_led_set;

    if (led_classdev_register(NULL, &pwr_led))
        pr_warn("rpi-fw: LED class registration failed\n");

    pr_info("rpi-fw: ready\n");
    return 0;
}

static void __exit rpi_fw_exit(void)
{
    led_classdev_unregister(&pwr_led);
    if (hwmon_dev)
        hwmon_device_unregister(hwmon_dev);
    iounmap(mbox_virt);
}

module_init(rpi_fw_init);
module_exit(rpi_fw_exit);

MODULE_AUTHOR("Steve Crawford");
MODULE_DESCRIPTION("RPi4 firmware temp/LED via direct VC mailbox (UEFI/ACPI boot)");
MODULE_LICENSE("GPL");
