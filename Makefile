# OCSFS — Open Cluster Shared FileSystem
# Build system
#
# Targets:
#   all        Build everything
#   tools      Build mkfs.ocsfs and ocsfs-tool
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

# Tools
MKFS_SRC = $(TOOL_DIR)/mkfs_ocsfs.c
TOOL_SRC = $(TOOL_DIR)/ocsfs_tool.c
MKFS_BIN = mkfs.ocsfs
TOOL_BIN = ocsfs-tool

# Tests
TEST_SRC   = $(TEST_DIR)/test_ocsfs.c
TEST_BIN   = test_ocsfs

# ─── Rules ──────────────────────────────────────────────────

.PHONY: all tools test clean

all: tools test

tools: $(MKFS_BIN) $(TOOL_BIN)

$(MKFS_BIN): $(MKFS_SRC) $(COMMON_OBJS)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ $(LDFLAGS)
	@echo "  Built: $@"

$(TOOL_BIN): $(TOOL_SRC) $(COMMON_OBJS)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ $(LDFLAGS)
	@echo "  Built: $@"

$(TEST_BIN): $(TEST_SRC) $(COMMON_OBJS)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ $(LDFLAGS)
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
	rm -f $(COMMON_OBJS) $(MKFS_BIN) $(TOOL_BIN) $(TEST_BIN)
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
