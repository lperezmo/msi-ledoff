# SPDX-License-Identifier: GPL-2.0
ifneq ($(KERNELRELEASE),)

obj-m := msi_ledoff.o

else

KVER ?= $(shell uname -r)
KSRC ?= /lib/modules/$(KVER)/build

all: modules

modules:
	$(MAKE) -C $(KSRC) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KSRC) M=$(CURDIR) clean

.PHONY: all modules clean

endif
