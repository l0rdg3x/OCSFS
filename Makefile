# OCSFS — Open Cluster Shared FileSystem
# Build system
#
# Targets:
#   all        Build everything (tools + test)
#   tools      Build mkfs.ocsfs and ocsfs-tool
#   fuse       Build ocsfs-fuse (requires libfuse3-dev)
#   test       Build and run tests
#   clean      Remove build artifacts
#
# SPDX-License-Identifier: GPL-2.0-only

CC       = gcc
CFLAGS   = -Wall -Wextra -Werror -std=gnu11 -O2 -g
CFLAGS  += -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64
INCLUDES = -I include

LDFLAGS  = -luuid -lpthread

# Source files
SRC_DIR  = src
TOOL_DIR = tools
TEST_DIR = tests

COMMON_SRCS = $(SRC_DIR)/crc32c.c \
              $(SRC_DIR)/bitmap.c \
              $(SRC_DIR)/extent.c \
              $(SRC_DIR)/lock.c \
              $(SRC_DIR)/heartbeat.c

COMMON_OBJS = $(COMMON_SRCS:.c=.o)

# New Phase 0 modules
PHASE0_SRCS = $(SRC_DIR)/btree.c \
              $(SRC_DIR)/inode.c \
              $(SRC_DIR)/journal.c \
              $(SRC_DIR)/dir.c

PHASE0_OBJS = $(PHASE0_SRCS:.c=.o)

# Tools
MKFS_SRC = $(TOOL_DIR)/mkfs_ocsfs.c
TOOL_SRC = $(TOOL_DIR)/ocsfs_tool.c
MKFS_BIN = mkfs.ocsfs
TOOL_BIN = ocsfs-tool

# FUSE
FUSE_SRC = $(SRC_DIR)/fuse_main.c
FUSE_BIN = ocsfs-fuse
FUSE_CFLAGS = $(shell pkg-config --cflags fuse3 2>/dev/null)
FUSE_LDFLAGS = $(shell pkg-config --libs fuse3 2>/dev/null)

# Tests
TEST_SRC   = $(TEST_DIR)/test_ocsfs.c
TEST_BIN   = test_ocsfs

# ─── Rules ──────────────────────────────────────────────────

.PHONY: all tools fuse test clean

all: tools test

tools: $(MKFS_BIN) $(TOOL_BIN)

$(MKFS_BIN): $(MKFS_SRC) $(COMMON_OBJS)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ $(LDFLAGS)
	@echo "  Built: $@"

$(TOOL_BIN): $(TOOL_SRC) $(COMMON_OBJS)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ $(LDFLAGS)
	@echo "  Built: $@"

$(TEST_BIN): $(TEST_SRC) $(COMMON_OBJS) $(PHASE0_OBJS)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ $(LDFLAGS)
	@echo "  Built: $@"

fuse: $(FUSE_BIN)

$(FUSE_BIN): $(FUSE_SRC) $(COMMON_OBJS)
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) $(INCLUDES) -o $@ $^ $(LDFLAGS) $(FUSE_LDFLAGS)
	@echo "  Built: $@"

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

test: $(TEST_BIN)
	@echo ""
	@echo "═══════════════════════════════════════════════"
	@echo "  Running OCSFS test suite..."
	@echo "═══════════════════════════════════════════════"
	@echo ""
	./$(TEST_BIN)

clean:
	rm -f $(COMMON_OBJS) $(PHASE0_OBJS) $(MKFS_BIN) $(TOOL_BIN) $(TEST_BIN) $(FUSE_BIN)
	rm -f /tmp/ocsfs_test_*.img
	@echo "  Cleaned."

# ─── Development helpers ────────────────────────────────────

# Create a test image and format it
demo: tools
	@echo "Creating 1 GiB test image..."
	dd if=/dev/zero of=/tmp/ocsfs_demo.img bs=1M count=1024 2>/dev/null
	./$(MKFS_BIN) -L "demo-volume" -N 8 -v -f /tmp/ocsfs_demo.img
	@echo ""
	./$(TOOL_BIN) info /tmp/ocsfs_demo.img
	./$(TOOL_BIN) df /tmp/ocsfs_demo.img
	./$(TOOL_BIN) check /tmp/ocsfs_demo.img

# Mount a test image with FUSE
demo-fuse: fuse tools
	@mkdir -p /tmp/ocsfs_mnt
	dd if=/dev/zero of=/tmp/ocsfs_fuse.img bs=1M count=512 2>/dev/null
	./$(MKFS_BIN) -L "fuse-test" -N 2 -J 4M -A 128M -f /tmp/ocsfs_fuse.img
	./$(FUSE_BIN) /tmp/ocsfs_fuse.img /tmp/ocsfs_mnt -f -o allow_other &
	@echo "Mounted at /tmp/ocsfs_mnt (PID $$!)"
