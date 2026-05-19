obj-m += hello.o
obj-m += monster.o
obj-m += monster2.o
obj-m += read_write.o

KDIR         := /home/felix/lemon-image/build/6.12.y
ARCH         := arm
CROSS_COMPILE := arm-none-linux-gnueabihf-

# Default target: build all kernel modules + the userspace test binary
all: modules test_user

# ── Kernel modules ──────────────────────────────────────────────────────────
modules:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

# ── Userspace test program (cross-compiled for ARM) ─────────────────────────
# Produces the ELF binary "test_user" to be copied to the target board.
test_user: test_user.c
	$(CROSS_COMPILE)gcc -Wall -Wextra -o test_user test_user.c

# ── Clean ────────────────────────────────────────────────────────────────────
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f test_user