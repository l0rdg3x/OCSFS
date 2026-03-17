# OCSFS — Out-of-tree kernel module build
# SPDX-License-Identifier: GPL-2.0-only
#
# Usage:
#   make                  Build the module
#   make clean            Clean build artifacts
#   make install          Install via DKMS
#
# Requires: linux-headers-$(uname -r)

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

# When building out-of-tree, CONFIG_OCSFS_FS=m
obj-m += ocsfs.o
ocsfs-y := super.o inode.o dir.o file.o extent.o journal.o bitmap.o

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a

.PHONY: all clean install
