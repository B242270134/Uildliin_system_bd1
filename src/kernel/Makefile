# Гурван module-ийг нэг дор build хийнэ.
#   make            -> tasklist_linear.ko, tasklist_dfs.ko, tasktree.ko
#   make clean
#   make KDIR=/path/to/headers   (өөр kernel-д build хийх бол)
obj-m += tasklist_linear.o
obj-m += tasklist_dfs.o
obj-m += tasktree.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
