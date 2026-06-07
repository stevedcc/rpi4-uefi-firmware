obj-m := rpi-pwr-led.o

rpi-pwr-led-objs := rpi-dt-populate.o

KVER ?= 7.0.8-200.fc44.aarch64
KDIR ?= $(PWD)/kernel-devel/usr/src/kernels/$(KVER)
ARCH ?= arm64
CROSS_COMPILE ?= aarch64-linux-gnu-

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules \
		ARCH=$(ARCH) \
		CROSS_COMPILE=$(CROSS_COMPILE)

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean \
		ARCH=$(ARCH) \
		CROSS_COMPILE=$(CROSS_COMPILE)
