# Svm-core
KDIR := /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

obj-m += svm-core.o
svm-core-objs := ssvm_main.o ssvm_ghost.o ssvm_vmexit.o ssvm_asm.o npt_walk.o

ccflags-y += -Wall -Wextra -Werror \
             -Wundef -Wunused \
             -Wnull-dereference -Wformat=2

ccflags-y += -g -fno-omit-frame-pointer -fstack-protector-strong

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean 2>/dev/null || true

.PHONY: all clean
