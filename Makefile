# modore — top-level build orchestrator.
#
# Detects the host platform and delegates to the platform-specific build
# under native/<platform>/. The Mozc bridge (bridge/) is shared across
# platforms and gets built by the per-platform Makefile via CMake.
#
# Plain `make` prints the target list — see `help` below. Use `make build`
# (or an explicit `make macos` / `make bridge`) to actually compile.

MODORE_ENABLE_SCRIPTING ?= 0
# Opt-in atzc relay backend (Wine-hosted engine over a Unix socket). See the
# `bridge` / `fetch-atzc` targets below.
MODORE_ENABLE_ATZC ?= 0

ifeq ($(OS),Windows_NT)
    PLATFORM := windows
else
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
endif

NATIVE_DIR := native/$(PLATFORM)
BRIDGE_DIR := bridge

# Mozc's build scripts hardcode `python` (no `3`). Prepend our shim dir for
# every sub-make so any CMake invocation downstream picks it up.
export PATH := $(CURDIR)/$(BRIDGE_DIR)/build-tools:$(PATH)

.PHONY: help build run open bridge clean clean-bridge distclean signing \
        check-platform macos linux windows install-user-bin test-puppeteer \
        fetch-luajit fetch-atzc engine engine-test clean-engine \
        macos-e2e macos-e2e-smoke macos-e2e-full macos-e2e-quarantine

.DEFAULT_GOAL := help

build: check-platform
	@$(MAKE) --no-print-directory -C $(NATIVE_DIR) MODORE_ENABLE_SCRIPTING=$(MODORE_ENABLE_SCRIPTING)

# Convenience: explicit per-OS targets (CI matrices, multi-OS machines).
macos:
	@$(MAKE) --no-print-directory -C native/macos MODORE_ENABLE_SCRIPTING=$(MODORE_ENABLE_SCRIPTING)

linux:
	@$(MAKE) --no-print-directory -C native/linux MODORE_ENABLE_SCRIPTING=$(MODORE_ENABLE_SCRIPTING)

windows:
	@$(MAKE) --no-print-directory -C native/windows MODORE_ENABLE_SCRIPTING=$(MODORE_ENABLE_SCRIPTING)

# Linux: install ~/.local/bin/modore-host + ~/.local/lib/modore/*.so
# Same as: make -C native/linux install-user-bin (from this repo root, not …/modore/modore/…).
install-user-bin: check-platform
ifneq ($(PLATFORM),linux)
	@echo "install-user-bin is Linux-only (host platform: $(PLATFORM))"; exit 1
else
	@$(MAKE) --no-print-directory -C native/linux install-user-bin
endif

# Linux: Puppeteer smoke test (needs modore-host running + graphical session; `npm ci` in test/puppeteer).
# Uses a system Chromium/Chrome via puppeteer-core; set
# PUPPETEER_EXECUTABLE_PATH if auto-detection misses your install.
test-puppeteer: check-platform
ifneq ($(PLATFORM),linux)
	@echo "test-puppeteer is Linux-only (host platform: $(PLATFORM))"; exit 1
else
	@cd test/puppeteer && npm ci && npm test
endif

# macOS GUI integration tests. Opt-in only: launches real apps and requires
# Accessibility permission for the host + invoking terminal/helper.
# No npm install step here: the runner is plain Node + swiftc.
macos-e2e: check-platform
ifneq ($(PLATFORM),macos)
	@echo "macos-e2e is macOS-only (host platform: $(PLATFORM))"; exit 1
else
	@$(MAKE) --no-print-directory build
	@cd test/macos-e2e && node runner.mjs
endif

macos-e2e-smoke: check-platform
ifneq ($(PLATFORM),macos)
	@echo "macos-e2e-smoke is macOS-only (host platform: $(PLATFORM))"; exit 1
else
	@$(MAKE) --no-print-directory build
	@cd test/macos-e2e && MODORE_E2E_SUITE=smoke node runner.mjs
endif

macos-e2e-full: check-platform
ifneq ($(PLATFORM),macos)
	@echo "macos-e2e-full is macOS-only (host platform: $(PLATFORM))"; exit 1
else
	@$(MAKE) --no-print-directory build
	@cd test/macos-e2e && MODORE_E2E_SUITE=full node runner.mjs
endif

macos-e2e-quarantine: check-platform
ifneq ($(PLATFORM),macos)
	@echo "macos-e2e-quarantine is macOS-only (host platform: $(PLATFORM))"; exit 1
else
	@$(MAKE) --no-print-directory build
	@cd test/macos-e2e && MODORE_E2E_SUITE=quarantine node runner.mjs
endif

run: check-platform
	@$(MAKE) --no-print-directory -C $(NATIVE_DIR) run MODORE_ENABLE_SCRIPTING=$(MODORE_ENABLE_SCRIPTING)

open: check-platform
ifeq ($(PLATFORM),windows)
	@echo "make open is not defined on Windows; use 'make run' instead"; exit 1
else
	@$(MAKE) --no-print-directory -C $(NATIVE_DIR) open MODORE_ENABLE_SCRIPTING=$(MODORE_ENABLE_SCRIPTING)
endif

# ---- atzc relay backend (opt-in) -------------------------------------------
# Reaches a Wine-hosted engine via atzcd over a Unix socket. Off by default.
# Build with `make bridge MODORE_ENABLE_ATZC=1` after `make fetch-atzc` (or
# point MODORE_ATZC_DIR at an existing atzc-server checkout).
# Repo is noatdk/atzc; the checkout lives in atzc-server/ to match the atzc/
# C++ include namespace (include/atzc/client.h), not the repo name.
ATZC_DIR        := third_party/atzc-server
ATZC_REPO       := https://github.com/noatdk/atzc.git
ATZC_BRANCH     := main
# Pinned commit. Bump deliberately; do not let the branch drift.
ATZC_SHA        := 39a37a121064503891271e430bd0c7ad7417cf74
MODORE_ATZC_DIR ?= $(CURDIR)/$(ATZC_DIR)

# Build just the cross-platform bridge (no host UI). Useful when iterating
# on the C ABI from a non-native frontend.
bridge:
ifeq ($(MODORE_ENABLE_ATZC),1)
	@if [ ! -f "$(MODORE_ATZC_DIR)/include/atzc/client.h" ]; then \
	    echo "atzc client not found at $(MODORE_ATZC_DIR)."; \
	    echo "Run 'make fetch-atzc' or set MODORE_ATZC_DIR=<atzc-server checkout>."; \
	    exit 1; \
	fi
endif
	@cmake -S $(BRIDGE_DIR) -B $(BRIDGE_DIR)/build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release \
	    $(if $(filter 1,$(MODORE_ENABLE_ATZC)),-DMODORE_ENABLE_ATZC=ON -DMODORE_ATZC_DIR=$(MODORE_ATZC_DIR),-DMODORE_ENABLE_ATZC=OFF)
	@cmake --build $(BRIDGE_DIR)/build -j

# ---- scripting engine -------------------------------------------------------
# libmodore_script.{dylib,so}: scripting/classifier helper library, sibling to
# the Mozc bridge. Default builds keep classifier/text helpers only; set
# MODORE_ENABLE_SCRIPTING=1 to include Lua hooks. Only that path needs LuaJIT.

LUAJIT_DIR     := third_party/luajit
# GitHub mirror used for shallow-clone support (luajit.org git server
# advertises dumb HTTP, which rejects --depth=1).
LUAJIT_REPO    := https://github.com/LuaJIT/LuaJIT.git
LUAJIT_BRANCH  := v2.1
# Pinned commit on v2.1. Bump deliberately; do not let the branch drift.
LUAJIT_SHA     := 18b087cd2cd4ddc4a79782bf155383a689d5093d
ENGINE_DIR     := engine
ENGINE_BUILD   := build/engine

fetch-luajit:
	@if [ -d $(LUAJIT_DIR)/.git ]; then \
	    have=$$(git -C $(LUAJIT_DIR) rev-parse HEAD); \
	    if [ "$$have" = "$(LUAJIT_SHA)" ]; then \
	        echo "luajit: already at pinned SHA $(LUAJIT_SHA)"; \
	    else \
	        echo "luajit: re-pinning $$have -> $(LUAJIT_SHA)"; \
	        git -C $(LUAJIT_DIR) fetch --depth=1 origin $(LUAJIT_SHA) || \
	            git -C $(LUAJIT_DIR) fetch origin $(LUAJIT_BRANCH); \
	        git -C $(LUAJIT_DIR) checkout --detach $(LUAJIT_SHA); \
	    fi; \
	else \
	    echo "luajit: cloning $(LUAJIT_REPO) @ $(LUAJIT_SHA)"; \
	    git clone --branch $(LUAJIT_BRANCH) $(LUAJIT_REPO) $(LUAJIT_DIR); \
	    git -C $(LUAJIT_DIR) checkout --detach $(LUAJIT_SHA); \
	fi

# Lazy-clone the atzc client into third_party/ at the pinned SHA. Only needed
# when building the bridge with MODORE_ENABLE_ATZC=1.
fetch-atzc:
	@if [ -d $(ATZC_DIR)/.git ]; then \
	    have=$$(git -C $(ATZC_DIR) rev-parse HEAD); \
	    if [ "$$have" = "$(ATZC_SHA)" ]; then \
	        echo "atzc: already at pinned SHA $(ATZC_SHA)"; \
	    else \
	        echo "atzc: re-pinning $$have -> $(ATZC_SHA)"; \
	        git -C $(ATZC_DIR) fetch --depth=1 origin $(ATZC_SHA) || \
	            git -C $(ATZC_DIR) fetch origin $(ATZC_BRANCH); \
	        git -C $(ATZC_DIR) checkout --detach $(ATZC_SHA); \
	    fi; \
	else \
	    echo "atzc: cloning $(ATZC_REPO) @ $(ATZC_SHA)"; \
	    git clone --branch $(ATZC_BRANCH) $(ATZC_REPO) $(ATZC_DIR); \
	    git -C $(ATZC_DIR) checkout --detach $(ATZC_SHA); \
	fi

engine:
	@if [ "$(MODORE_ENABLE_SCRIPTING)" = "1" ] && [ ! -d $(LUAJIT_DIR)/.git ]; then \
	    echo "luajit not fetched. Run: make fetch-luajit"; exit 1; \
	fi
	@cmake -S $(ENGINE_DIR) -B $(ENGINE_BUILD) -G "Unix Makefiles" \
	    -DCMAKE_BUILD_TYPE=Release \
	    -DMODORE_SCRIPT_ENABLE_LUA=$(if $(filter 1,$(MODORE_ENABLE_SCRIPTING)),ON,OFF)
	@cmake --build $(ENGINE_BUILD) -j

engine-test: engine
	@ctest --test-dir $(ENGINE_BUILD) --output-on-failure

clean-engine:
	@rm -rf $(ENGINE_BUILD)

clean: check-platform
	@$(MAKE) --no-print-directory -C $(NATIVE_DIR) clean

clean-bridge:
	@rm -rf $(BRIDGE_DIR)/build

distclean: clean clean-bridge clean-engine
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
	 echo "Currently supported: Darwin (macOS), Linux (X11 host), and Windows."; \
	 exit 1
endif

help:
	@echo "modore build orchestrator (host: $(PLATFORM))"
	@echo
	@echo "Build:"
	@echo "  make build      build the host app for the current platform"
	@echo "                  add MODORE_ENABLE_SCRIPTING=1 to include Lua hooks"
	@echo "  make macos      build the macOS host explicitly"
	@echo "  make linux      build the Linux host explicitly"
	@echo "  make windows    build the Windows host explicitly"
	@echo "  make install-user-bin   Linux: install to ~/.local (modore-host + bridge .so)"
	@echo "  make bridge     build only the cross-platform Mozc bridge"
	@echo "                  add MODORE_ENABLE_ATZC=1 to also build the atzc relay backend"
	@echo "  make fetch-atzc lazy clone the atzc client into third_party/atzc-server/"
	@echo "  make fetch-luajit  lazy clone LuaJIT 2.1 into third_party/luajit/"
	@echo "  make engine     build libmodore_script; add MODORE_ENABLE_SCRIPTING=1 for Lua hooks"
	@echo "  make engine-test  run engine smoke harness"
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
	@echo "  make macos-e2e-smoke  macOS GUI E2E smoke suite (opens real apps)"
	@echo "  make macos-e2e-full   macOS GUI E2E full stable app suite"
	@echo "  make macos-e2e-quarantine  macOS GUI E2E quarantined app suite"
	@echo "  make help       print this list (default)"
