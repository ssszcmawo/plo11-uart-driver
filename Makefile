obj-m += plo11.o

KDIR := ~/buildroot/output/build/linux-6.18.7

ARCH := arm64
CROSS_COMPILE := ~/buildroot/output/host/bin/aarch64-buildroot-linux-gnu-

all:
	make -C $(KDIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

clean:
	make -C $(KDIR) M=$(PWD) clean

