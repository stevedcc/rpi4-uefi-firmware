# rpi4-uefi-firmware

Kernel module that exposes Raspberry Pi 4 firmware services on systems booted
via UEFI+ACPI — such as Fedora CoreOS.

After loading:

```
/sys/class/leds/PWR/brightness      write 0 = LED off, 1 = LED on
/sys/class/hwmon/hwmonN/temp1_input SoC temperature in millidegrees C
```

## Problem

On Raspberry Pi 4 booted via UEFI (e.g. using the
[rpi4-uefi firmware](https://github.com/pftf/RPi4)), the kernel uses ACPI for
device discovery and never populates the OF device tree. The standard driver
chain:

```
bcm2835-mbox → raspberrypi-firmware → firmware-gpio → PWR LED / hwmon
```

never initialises, even though all those drivers are built into the Fedora
kernel and the DT nodes are present and enabled in `bcm2711-rpi-4-b.dtb`.

The result: the red PWR LED stays on permanently and the SoC temperature is
not accessible via the firmware interface.

> **Note**: SoC temperature *is* available via the ACPI thermal zone
> (`/sys/class/hwmon/hwmon0/temp1_input`, name `acpitz`) on Fedora CoreOS
> without this module. This module's primary value is PWR LED control and as
> a foundation for a proper upstream fix.

## How it works

This module drives the BCM2711 VideoCore mailbox directly via the property
interface (channel 8), without going through the kernel driver stack.

Key findings from reading the Raspberry Pi kernel source
(`drivers/gpio/gpio-raspberrypi-exp.c`):

- Expander GPIOs are numbered from **128** (`RPI_EXP_GPIO_BASE`) in the
  firmware namespace. PWR LED = expgpio pin 2 = firmware GPIO **130**.
- `SET_GPIO_CONFIG` (tag `0x00038043`) must be called to configure the pin as
  an output **before** `SET_GPIO_STATE` (tag `0x00038041`) has any effect.
- The mailbox buffer must be in the first 1 GB of ARM RAM (VC bus alias
  `0xC0000000`). `GFP_DMA` works because `ZONE_DMA` covers exactly 1 GB on
  this kernel.
- The ARM D-cache must be explicitly flushed (`DC CIVAC`) before and after
  each mailbox transaction — the buffer is in cached memory but the
  VideoCore reads physical RAM directly.

## Building

Cross-compile from an x86_64 Fedora host. The kernel headers for the target
kernel are not in the x86_64 repos, so they must be fetched from Koji and
the host build tools substituted in.

```bash
# Install cross-compiler
sudo dnf install gcc-aarch64-linux-gnu

# Set the target kernel version
KVER=7.0.8-200.fc44.aarch64          # uname -r on the Pi nodes
KVER_VER=${KVER%%-*}                  # 7.0.8
KVER_REL=${KVER#*-}                   # 200.fc44.aarch64
KVER_REL=${KVER_REL%.*}               # 200.fc44
KVER_ARCH=${KVER##*.}                 # aarch64

# Download and extract aarch64 kernel headers
curl -O "https://kojipkgs.fedoraproject.org/packages/kernel/${KVER_VER}/${KVER_REL}/${KVER_ARCH}/kernel-devel-${KVER}.rpm"
mkdir -p kernel-devel
cd kernel-devel && rpm2cpio ../kernel-devel-${KVER}.rpm | cpio -idm --quiet && cd ..

# The RPM ships aarch64 host build tools — replace with native x86_64 ones
KDIR=kernel-devel/usr/src/kernels/${KVER}
cp /usr/src/kernels/$(uname -r)/scripts/basic/fixdep ${KDIR}/scripts/basic/fixdep
cp /usr/src/kernels/$(uname -r)/scripts/mod/modpost  ${KDIR}/scripts/mod/modpost

# Generate autoconf.h from the running kernel config on a Pi node
ssh core@<node> "cat /lib/modules/${KVER}/config" | python3 -c "
import sys
print('/* Automatically generated */\n#ifndef __GENERATED_AUTOCONF_H__\n#define __GENERATED_AUTOCONF_H__')
for line in sys.stdin:
    line = line.rstrip()
    if line.startswith('# CONFIG_') and line.endswith(' is not set'):
        print('/* {} */'.format(line[2:]))
    elif line.startswith('CONFIG_'):
        k, _, v = line.partition('=')
        if v in ('y', 'm'): print('#define {} 1'.format(k))
        elif v != 'n': print('#define {} {}'.format(k, v))
print('#endif')
" > ${KDIR}/include/generated/autoconf.h

make
```

## Installing

Run this for each node. The `chcon` step is required — see SELinux note below.

```bash
NODE=core@<node>
KVER=$(ssh $NODE "uname -r")

scp rpi-pwr-led.ko $NODE:
ssh $NODE "
  sudo mkdir -p /usr/local/lib/modules/${KVER}
  sudo cp ~/rpi-pwr-led.ko /usr/local/lib/modules/${KVER}/
  sudo chcon -t modules_object_t /usr/local/lib/modules/${KVER}/rpi-pwr-led.ko
"
```

Create the systemd unit on the node:

```bash
ssh $NODE "sudo tee /etc/systemd/system/rpi-fw.service > /dev/null" << 'EOF'
[Unit]
Description=Load RPi firmware module (PWR LED off + temperature)
After=local-fs.target

[Service]
Type=oneshot
ExecStart=insmod /usr/local/lib/modules/%v/rpi-pwr-led.ko
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF
```

Enable and start:

```bash
ssh $NODE "sudo systemctl daemon-reload && sudo systemctl enable --now rpi-fw.service"
```

> **SELinux note**: the `chcon -t modules_object_t` step is required on
> Fedora CoreOS. Without it the service fails with *Permission denied* at
> boot. This happens because the systemd service runs in the `system_r`
> SELinux context, while interactive `sudo insmod` runs in `unconfined_r`
> and is not subject to the same type enforcement.

## Kernel updates

The `.ko` is compiled for a specific kernel version. After a kernel update,
rebuild against the new kernel headers and redeploy. The install path includes
the kernel version so old and new can coexist; the service uses systemd's `%v`
specifier (equivalent to `uname -r`) to load the correct one automatically.

## Upstream status

This module is a workaround pending a proper upstream fix. The right solutions
are:

1. **Add ACPI IDs to `bcm2835-mbox`** so the mailbox driver binds under
   UEFI+ACPI boot without any DT involvement, allowing the existing
   `raspberrypi-firmware` MFD chain to initialise normally.
2. **Ship this module** in `drivers/platform/raspberrypi/` as a fallback for
   UEFI-booted systems that lack the DT-based driver chain.

Contributions and testing on other UEFI-booted Pi4 distributions welcome.

## Tested on

- Fedora CoreOS 44 (`7.0.8-200.fc44.aarch64`) on Raspberry Pi 4B (8 GB)
