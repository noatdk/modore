# modore — top-level build orchestrator.
#
# Detects the host platform and delegates to the platform-specific build
# under native/<platform>/. The Mozc bridge (bridge/) is shared across
# platforms and gets built by the per-platform Makefile via CMake.
#
# Plain `make` prints the target list — see `help` below. Use `make build`
# (or an explicit `make macos` / `make bridge`) to actually compile.

UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
    PLATFORM := macos
else ifeq ($(UNAME_S),Linux)
    PLATFORM := linux
else ifneq (,$(findstring MINGW,$(UNAME_S)))
    PLATFORM := windows
else ifneq (,$(findstring MSYS,$(UNAME_S)))
    PLATFORM := windows
else
    PLATFORM := unsupported
endif

NATIVE_DIR := native/$(PLATFORM)
BRIDGE_DIR := bridge

# Mozc's build scripts hardcode `python` (no `3`). Prepend our shim dir for
# every sub-make so any CMake invocation downstream picks it up.
export PATH := $(CURDIR)/$(BRIDGE_DIR)/build-tools:$(PATH)

.PHONY: help build run open bridge clean clean-bridge distclean signing \
        check-platform macos linux install-user-bin test-puppeteer

.DEFAULT_GOAL := help

build: check-platform
	@$(MAKE) --no-print-directory -C $(NATIVE_DIR)

# Convenience: explicit per-OS targets (CI matrices, multi-OS machines).
macos:
	@$(MAKE) --no-print-directory -C native/macos

linux:
	@$(MAKE) --no-print-directory -C native/linux

# Linux: install ~/.local/bin/modore-host + ~/.local/lib/modore/*.so
# Same as: make -C native/linux install-user-bin (from this repo root, not …/modore/modore/…).
install-user-bin: check-platform
ifneq ($(PLATFORM),linux)
	@echo "install-user-bin is Linux-only (host platform: $(PLATFORM))"; exit 1
else
	@$(MAKE) --no-print-directory -C native/linux install-user-bin
endif

# Linux: Puppeteer smoke test (needs modore-host running + graphical session; `npm ci` in test/puppeteer).
test-puppeteer: check-platform
ifneq ($(PLATFORM),linux)
	@echo "test-puppeteer is Linux-only (host platform: $(PLATFORM))"; exit 1
else
	@cd test/puppeteer && npm ci && npm test
endif

run: check-platform
	@$(MAKE) --no-print-directory -C $(NATIVE_DIR) run

open: check-platform
	@$(MAKE) --no-print-directory -C $(NATIVE_DIR) open

# Build just the cross-platform bridge (no host UI). Useful when iterating
# on the C ABI from a non-native frontend.
bridge:
	@cmake -S $(BRIDGE_DIR) -B $(BRIDGE_DIR)/build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
	@cmake --build $(BRIDGE_DIR)/build -j

clean: check-platform
	@$(MAKE) --no-print-directory -C $(NATIVE_DIR) clean

clean-bridge:
	@rm -rf $(BRIDGE_DIR)/build

distclean: clean clean-bridge
	@echo "Wiped all build outputs. Next 'make' will rebuild Mozc (~5 min)."

# One-time setup: create the self-signed code-signing identity macOS needs
# so TCC (Accessibility permission) recognizes the app across rebuilds.
signing:
ifeq ($(PLATFORM),macos)
	@bash native/macos/scripts/setup-signing.sh
else
	@echo "make signing is macOS-only (host platform: $(PLATFORM))"; exit 1
endif

check-platform:
ifeq ($(PLATFORM),unsupported)
	@echo "Unsupported host platform: $(UNAME_S)"; \
	 echo "Currently supported: Darwin (macOS) and Linux (X11 host)."; \
	 exit 1
endif
ifeq ($(PLATFORM),windows)
	@echo "Windows host not wired up yet. The bridge builds cross-platform"; \
	 echo "(see 'make bridge'), but no Windows frontend exists in this repo."; \
	 exit 1
endif

help:
	@echo "modore build orchestrator (host: $(PLATFORM))"
	@echo
	@echo "Build:"
	@echo "  make build      build the host app for the current platform"
	@echo "  make macos      build the macOS host explicitly"
	@echo "  make linux      build the Linux host explicitly"
	@echo "  make install-user-bin   Linux: install to ~/.local (modore-host + bridge .so)"
	@echo "  make bridge     build only the cross-platform Mozc bridge"
	@echo
	@echo "Run:"
	@echo "  make run        build and launch the host app"
	@echo "  make open       build and 'open' the .app bundle (macOS)"
	@echo
	@echo "Clean:"
	@echo "  make clean      wipe per-platform build outputs"
	@echo "  make distclean  also wipe bridge/build (forces ~5min rebuild)"
	@echo
	@echo "Other:"
	@echo "  make signing    one-time: create self-signed identity (macOS)"
	@echo "  make test-puppeteer  Linux: Chromium E2E vs running modore-host (see test/puppeteer/)"
	@echo "  make help       print this list (default)"
