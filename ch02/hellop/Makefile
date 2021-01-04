ifneq ($(KERNELRELEASE),)
	MOD_NAME := hello
	obj-m := $(MOD_NAME).o
else
	KERNELDIR ?= /lib/modules/$(shell uname -r)/build
	PWD := $(shell pwd)

default:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules
endif

clean:
	rm -rf *.o .*.cmd *.ko *.mod *.mod.c *.order *.symvers
