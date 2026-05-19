obj-m += hello.o

KDIR := /home/felix/lemon-image/build/6.12.y
ARCH := arm
CROSS_COMPILE := arm-none-linux-gnueabihf-

all:
	make -C $(KDIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

clean:
	make -C $(KDIR) M=$(PWD) clean