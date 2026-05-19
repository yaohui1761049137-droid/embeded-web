# Makefile for LubanCat RK3566 Boa Web Server
# Host: x86_64, Target: aarch64 (ARM64)

CC = aarch64-linux-gnu-gcc
STRIP = aarch64-linux-gnu-strip
CFLAGS = -Wall -O2
LDFLAGS =

SRC_DIR = src
BUILD_DIR = build
PKG_DIR = package
BOA_DIR = boa-0.94.13

CGI_SRCS = $(wildcard $(SRC_DIR)/*.cgi.c)
CGI_BINS = $(patsubst $(SRC_DIR)/%.cgi.c,$(BUILD_DIR)/cgi/%,$(CGI_SRCS))

# Exclude common.c from CGI binaries
CGI_BINS_FILTERED = $(filter-out $(BUILD_DIR)/cgi/common,$(CGI_BINS))

.PHONY: all clean deploy setup-boa build-boa build-cgi

all: setup-boa build-boa build-cgi package

# === Toolchain check ===
check-toolchain:
	@which $(CC) > /dev/null 2>&1 || (echo "ERROR: $(CC) not found. Install: sudo apt install gcc-aarch64-linux-gnu"; exit 1)

# === Step 1: Setup Boa source ===
setup-boa: check-toolchain
	@if [ ! -f "$(BOA_DIR)/src/boa.c" ]; then \
		echo "Downloading Boa 0.94.13..."; \
		wget -q -O /tmp/boa-0.94.13.tar.gz http://www.boa.org/boa-0.94.13.tar.gz 2>/dev/null || \
		wget -q -O /tmp/boa-0.94.13.tar.gz https://sourceforge.net/projects/boa/files/boa/0.94.13/boa-0.94.13.tar.gz 2>/dev/null || \
		(echo "ERROR: Cannot download Boa. Manually download from http://www.boa.org/ and extract to $(BOA_DIR)/"; exit 1); \
		tar xzf /tmp/boa-0.94.13.tar.gz; \
	fi
	@echo "Boa source ready at $(BOA_DIR)/"

# === Step 2: Cross-compile Boa ===
build-boa: setup-boa
	@echo "Compiling Boa for aarch64..."
	@$(MAKE) -C $(BOA_DIR)/src CC=$(CC) STRIP=$(STRIP) 2>&1 && \
		mkdir -p $(BUILD_DIR)/boa && \
		cp $(BOA_DIR)/src/boa $(BUILD_DIR)/boa/ && \
		echo "Boa compiled successfully: $(BUILD_DIR)/boa/boa" && \
		file $(BUILD_DIR)/boa/boa

# === Step 3: Build CGI programs ===
$(BUILD_DIR)/cgi/%: $(SRC_DIR)/%.cgi.c $(SRC_DIR)/common.c $(SRC_DIR)/common.h
	@mkdir -p $(BUILD_DIR)/cgi
	$(CC) $(CFLAGS) -o $@ $< $(SRC_DIR)/common.c $(LDFLAGS)

build-cgi: check-toolchain $(CGI_BINS_FILTERED)
	@echo "CGI programs built:"
	@for f in $(CGI_BINS_FILTERED); do \
		file $$$f | grep -q "ARM aarch64" && echo "  $$f [OK]" || echo "  $$f [ERROR: not aarch64]"; \
	done

# === Step 4: Package ===
package: build-boa build-cgi
	@echo "Packaging deployment files..."
	@mkdir -p $(PKG_DIR)/bin $(PKG_DIR)/cgi-bin $(PKG_DIR)/www $(PKG_DIR)/etc
	@cp $(BUILD_DIR)/boa/boa $(PKG_DIR)/bin/
	@cp $(BUILD_DIR)/cgi/* $(PKG_DIR)/cgi-bin/ 2>/dev/null || true
	@chmod +x $(PKG_DIR)/cgi-bin/*
	@echo "Package ready in $(PKG_DIR)/"
	@echo ""
	@echo "=== Build Summary ==="
	@echo "Boa binary:"
	@file $(PKG_DIR)/bin/boa
	@echo ""
	@echo "CGI binaries:"
	@for f in $(PKG_DIR)/cgi-bin/*; do \
		if [ -f "$$f" ]; then \
			file "$$f" | grep -o "ELF.*aarch64.*$$" || echo "  $$f [NOT aarch64!]"; \
		fi; \
	done
	@echo ""
	@echo "To deploy: cd $(PKG_DIR) && ./deploy.sh [board_ip]"

# === Clean ===
clean:
	-rm -rf $(BUILD_DIR)
	-$(MAKE) -C $(BOA_DIR)/src clean 2>/dev/null || true

distclean: clean
	-rm -rf $(BOA_DIR)
	-rm -f /tmp/boa-0.94.13.tar.gz

# === Deploy ===
deploy:
	@cd $(PKG_DIR) && bash deploy.sh
