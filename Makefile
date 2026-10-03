ifneq ($(KERNELRELEASE),)

obj-m += ap3216c_drv.o

else
# 根据实际情况指定内核源码目录（即用于编译.ko文件的顶层Makefile目录）
KDIR ?= /home/leslie/linux/IMX6ULL/linux/linux-imx-rel_4.1.15_2.1.0_alientek

ARCH ?= arm
CROSS_COMPILE ?= arm-linux-gnueabihf-

.PHONY: all modules ap3216c_monitor clean

all: modules ap3216c_monitor

modules:
	@echo "Building AP3216C kernel module..."
	@echo "KDIR=$(KDIR)"
	$(MAKE) -C $(KDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) M=$(CURDIR) modules

ap3216c_monitor: ap3216c_monitor.c
	@echo "Building userspace ap3216c_monitor program..."
	$(CROSS_COMPILE)gcc -O2 -Wall -Wextra -o $@ $<

clean:
	$(MAKE) -C $(KDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) M=$(CURDIR) clean
	$(RM) ap3216c_monitor

endif
