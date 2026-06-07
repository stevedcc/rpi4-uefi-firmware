# rpi4-uefi-firmware

Kernel module that exposes Raspberry Pi 4 firmware services (PWR LED control,
SoC temperature) on systems booted via UEFI+ACPI — such as Fedora CoreOS.

## Problem

On Raspberry Pi 4 booted via UEFI (e.g. using the [rpi4-uefi firmware](https://github.com/pftf/RPi4)),
the kernel uses ACPI for device discovery and never populates the Open Firmware
device tree. The standard driver chain:

```
bcm2835-mbox → raspberrypi-firmware → firmware-gpio → PWR LED / hwmon
```

never initialises, even though all those drivers are built into the Fedora
kernel and the DT nodes are present and correctly configured in
`bcm2711-rpi-4-b.dtb`.

The result: the red PWR LED stays on permanently and the SoC temperature is
not accessible via the firmware interface.

(Note: SoC temperature *is* available via the ACPI thermal zone at
`/sys/class/hwmon/hwmon0` on Fedora CoreOS — this module is primarily useful
for PWR LED control and as a stepping stone toward a proper upstream fix.)

## Solution

This module drives the BCM2711 VideoCore mailbox directly using the property
interface (channel 8), reproducing what `gpio-raspberrypi-exp.c` and
`raspberrypi-hwmon.c` do via the full driver stack.

Key findings from reading the Raspberry Pi kernel source:

- Expander GPIOs are numbered from **128** (`RPI_EXP_GPIO_BASE`) in the
  firmware namespace. PWR LED = expgpio pin 2 = firmware GPIO **130**.
- `SET_GPIO_CONFIG` (tag `0x00038043`) must be called to set direction=output
  **before** `SET_GPIO_STATE` (tag `0x00038041`) has any effect.
- The mailbox buffer must be in the first 1 GB of ARM RAM (VC bus alias
  `0xC0000000`). On Fedora CoreOS, `ZONE_DMA` covers exactly 1 GB, so
  `GFP_DMA` works.
- ARM D-cache must be explicitly flushed (`DC CIVAC`) before and after each
  mailbox transaction since the buffer is in cached memory.

## After loading

```
/sys/class/leds/PWR/brightness   — write 0 to turn LED off, 1 to turn on
/sys/class/hwmon/hwmonN/temp1_input  — SoC temperature in millidegrees C
```

## Building

Requires the aarch64 cross-compiler and kernel headers for the target kernel.
On Fedora (x86_64 host, aarch64 target):

```bash
sudo dnf install gcc-aarch64-linux-gnu

# Download aarch64 kernel-devel from Koji and extract locally
KVER=7.0.8-200.fc44.aarch64
curl -O "https://kojipkgs.fedoraproject.org/packages/kernel/${KVER%%-*}/${KVER##*-}/aarch64/kernel-devel-${KVER}.rpm"
mkdir kernel-devel && cd kernel-devel
rpm2cpio ../kernel-devel-${KVER}.rpm | cpio -idm --quiet
cd ..

# Copy x86_64 host tools into the extracted headers (they ship as aarch64 binaries)
cp /usr/src/kernels/$(uname -r)/scripts/basic/fixdep kernel-devel/usr/src/kernels/${KVER}/scripts/basic/fixdep
cp /usr/src/kernels/$(uname -r)/scripts/mod/modpost  kernel-devel/usr/src/kernels/${KVER}/scripts/mod/modpost

# Generate autoconf.h from the running target kernel
ssh core@<node> "cat /lib/modules/${KVER}/config" | python3 -c "
import sys
print('/* Automatically generated */\n#ifndef __GENERATED_AUTOCONF_H__\n#define __GENERATED_AUTOCONF_H__')
for line in sys.stdin:
    line = line.rstrip()
    if line.startswith('# CONFIG_') and line.endswith(' is not set'):
        print('/* {} */'.format(line[2:]))
    elif line.startswith('CONFIG_'):
        k, _, v = line.partition('=')
        if v in ('y','m'): print('#define {} 1'.format(k))
        elif v != 'n': print('#define {} {}'.format(k, v))
print('#endif')
" > kernel-devel/usr/src/kernels/${KVER}/include/generated/autoconf.h

make
```

## Deploying

```bash
scp rpi-pwr-led.ko core@<node>:
ssh core@<node> "sudo mkdir -p /usr/local/lib/modules/\$(uname -r) && \
                 sudo cp rpi-pwr-led.ko /usr/local/lib/modules/\$(uname -r)/ && \
                 sudo chcon -t modules_object_t /usr/local/lib/modules/\$(uname -r)/rpi-pwr-led.ko"
```

> **SELinux note**: the `chcon` step is required on Fedora CoreOS. Without it,
> the systemd service will fail with *Permission denied* at boot even though
> `sudo insmod` works interactively (different SELinux context).

Create `/etc/systemd/system/rpi-fw.service`:

```ini
[Unit]
Description=Load RPi firmware module (PWR LED off + temperature)
DefaultDependencies=no
After=local-fs.target

[Service]
Type=oneshot
ExecStart=/bin/sh -c 'insmod /usr/local/lib/modules/$(uname -r)/rpi-pwr-led.ko 2>/dev/null || true'
RemainAfterExit=yes

[Install]
WantedBy=sysinit.target
```

```bash
sudo systemctl enable --now rpi-fw.service
```

## Upstream status

This module is a workaround. The correct fix is one of:

1. Add ACPI device IDs to `bcm2835-mbox` so the mailbox driver binds under
   UEFI+ACPI boot, allowing the existing `raspberrypi-firmware` MFD chain to
   work without any out-of-tree code.
2. Ship this module (or an equivalent) in `drivers/platform/raspberrypi/`
   as a fallback for UEFI-booted systems.

Contributions and testing on other UEFI-booted Pi4 distributions welcome.

## Tested on

- Fedora CoreOS 44 (`7.0.8-200.fc44.aarch64`) on Raspberry Pi 4B (8 GB)
