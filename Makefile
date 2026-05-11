# modeless-ime — top-level build orchestrator.
#
# Detects the host platform and delegates to the platform-specific build
# under native/<platform>/. The Mozc bridge (bridge/) is shared across
# platforms and gets built by the per-platform Makefile via CMake.
#
# Common targets:
#   make            — build the host app for the current platform (alias: build)
#   make run        — build and launch the host app
#   make bridge     — build only the cross-platform Mozc bridge (libmozc_bridge)
#   make clean      — wipe per-platform build outputs (keeps bridge/build)
#   make distclean  — also wipe bridge/build (forces a ~5min rebuild)
#   make signing    — one-time: create the self-signed identity for codesign
#   make help       — print this list

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

.PHONY: all build run open bridge clean clean-bridge distclean signing help \
        check-platform macos

all: build

build: check-platform
	@$(MAKE) --no-print-directory -C $(NATIVE_DIR)

# Convenience: explicit macOS target. Useful in CI matrices or when a
# developer is on a multi-OS machine and wants to be explicit.
macos:
	@$(MAKE) --no-print-directory -C native/macos

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
	 echo "Currently supported: Darwin (macOS). Linux/Windows hosts not wired up yet."; \
	 exit 1
endif
ifeq ($(PLATFORM),linux)
	@echo "Linux host not wired up yet. The bridge builds cross-platform"; \
	 echo "(see 'make bridge'), but no Linux frontend exists in this repo."; \
	 exit 1
endif
ifeq ($(PLATFORM),windows)
	@echo "Windows host not wired up yet. The bridge builds cross-platform"; \
	 echo "(see 'make bridge'), but no Windows frontend exists in this repo."; \
	 exit 1
endif

help:
	@echo "modeless-ime build orchestrator (host: $(PLATFORM))"
	@echo
	@echo "  make            build the host app (default)"
	@echo "  make run        build and launch the host app"
	@echo "  make open       build and 'open' the .app bundle (macOS)"
	@echo "  make bridge     build only the cross-platform Mozc bridge"
	@echo "  make clean      wipe per-platform build outputs"
	@echo "  make distclean  also wipe bridge/build (forces ~5min rebuild)"
	@echo "  make signing    one-time: create self-signed identity (macOS)"
	@echo "  make help       print this list"
