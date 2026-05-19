obj-m += hello.o
KDIR = "/home/felix/lemon-image/build/6.12.y"

all:
	make -C $(KDIR) M=$(PWD) modules
clean:
	make -C $(KDIR) M=$(PWD) clean