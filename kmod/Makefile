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
ocsfs-y := super.o inode.o dir.o dir_rename.o file.o \
           extent.o extent_btree.o \
           btree.o btree_mod.o \
           journal.o journal_replay.o \
           bitmap.o alloc.o \
           lock.o lock_io.o \
           heartbeat.o node.o recovery.o \
           iomap.o thin.o \
           snapshot.o refcount.o compress.o \
           scsi_pr.o xattr.o

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a

.PHONY: all clean install
