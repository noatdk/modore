# modore — Linux host (X11 + AT-SPI2 + in-process Mozc bridge).

REPO_ROOT := $(abspath ../..)
MODORE_ENABLE_SCRIPTING ?= 0
BRIDGE_SRC := $(REPO_ROOT)/bridge
BRIDGE_BUILD := $(BRIDGE_SRC)/build
BRIDGE_LIB := $(BRIDGE_BUILD)/libmozc_bridge.so

# Lua scripting engine (cmake project at engine/, LuaJIT vendored lazily).
ENGINE_SRC := $(REPO_ROOT)/engine
ENGINE_BUILD := $(REPO_ROOT)/build/engine
ENGINE_LIB := $(ENGINE_BUILD)/libmodore_script.so
SCRIPTING_CMAKE_FLAG := -DMODORE_SCRIPT_ENABLE_LUA=$(if $(filter 1,$(MODORE_ENABLE_SCRIPTING)),ON,OFF)

BUILD := build
OUT := $(BUILD)/modore-host
BRIDGE_SO := $(BUILD)/libmozc_bridge.so
ENGINE_SO := $(BUILD)/libmodore_script.so

CXX ?= g++
CLANG_FORMAT ?= clang-format

export PATH := $(REPO_ROOT)/bridge/build-tools:$(PATH)

PKG_ATSPI := $(shell pkg-config --cflags --libs atspi-2)
PKG_X11 := $(shell pkg-config --cflags --libs x11 xtst)

CXXFLAGS := -std=c++20 -O2 -Wall -Wextra \
           -I$(BRIDGE_SRC)/include -I$(ENGINE_SRC)/include -pthread
LDFLAGS := -pthread -Wl,-rpath,\$$ORIGIN
# gobject: g_object_ref/unref (atspi headers use GObject; link needs -lgobject).
PKG_GOBJECT := $(shell pkg-config --libs gobject-2.0)

SRC := main.cpp config.cpp evdev_hotkey.cpp ipc.cpp log.cpp scripting.cpp
HDR := config.hpp evdev_hotkey.hpp ipc.hpp log.hpp scripting.hpp
FORMAT_FILES := $(SRC) $(HDR)

PREFIX := $(HOME)/.local
INSTALL_LIB_DIR := $(PREFIX)/lib/modore
INSTALL_REAL_BIN := $(INSTALL_LIB_DIR)/modore-host
INSTALL_BIN := $(PREFIX)/bin/modore-host
SYSTEMD_USER_DIR := $(HOME)/.config/systemd/user
SYSTEMD_UNIT := $(CURDIR)/systemd/user/modore-host.service

.PHONY: all run run-local clean format install-user-bin bootstrap-evdev-access install-systemd-user reload-user-service

all: $(OUT)

format:
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

# Top-level orchestrator builds the shared bridge (CMake + Mozc).
$(BRIDGE_LIB):
	@$(MAKE) -C "$(REPO_ROOT)" bridge

# Shared helpers library. Default host builds keep classifier/text helpers
# and omit LuaJIT-backed scripting hooks. Set MODORE_ENABLE_SCRIPTING=1 to
# opt back in.
$(ENGINE_LIB):
	@if [ "$(MODORE_ENABLE_SCRIPTING)" = "1" ] && [ ! -d "$(REPO_ROOT)/third_party/luajit/.git" ]; then \
	    echo "scripting engine needs LuaJIT — run 'make fetch-luajit' at repo root"; exit 1; \
	fi
	cmake -S "$(ENGINE_SRC)" -B "$(ENGINE_BUILD)" -G "Unix Makefiles" \
		-DCMAKE_BUILD_TYPE=Release $(SCRIPTING_CMAKE_FLAG)
	cmake --build "$(ENGINE_BUILD)" -j --target modore_script

$(OUT): format $(SRC) $(BRIDGE_LIB) $(ENGINE_LIB) Makefile
	mkdir -p "$(BUILD)"
	cp -f "$(BRIDGE_LIB)" "$(BRIDGE_SO)"
	cp -f "$(ENGINE_LIB)" "$(ENGINE_SO)"
	$(CXX) $(CXXFLAGS) -o "$(OUT)" $(SRC) $(LDFLAGS) \
		-L"$(BUILD)" -lmozc_bridge -lmodore_script -lstdc++ \
		$(PKG_ATSPI) $(PKG_GOBJECT) $(PKG_X11)

run: install-systemd-user reload-user-service

run-local: $(OUT)
	"$(OUT)"

# Copies modore-host + libmozc_bridge.so + libmodore_script.so into
# ~/.local (rpath expects the .so beside the real binary;
# ~/.local/bin/modore-host is a symlink).
install-user-bin: $(OUT)
	install -d "$(INSTALL_LIB_DIR)" "$(PREFIX)/bin"
	install -m755 "$(OUT)" "$(INSTALL_REAL_BIN)"
	install -m755 "$(BRIDGE_SO)" "$(INSTALL_LIB_DIR)/libmozc_bridge.so"
	install -m755 "$(ENGINE_SO)" "$(INSTALL_LIB_DIR)/libmodore_script.so"
	ln -sf "$(INSTALL_REAL_BIN)" "$(INSTALL_BIN)"

bootstrap-evdev-access: install-user-bin
	@echo "WARNING: skipping file capabilities on $(INSTALL_REAL_BIN)"
	@echo "         file caps trigger secure-exec and break the user-service loader path"
	@echo "         hotkey access to /dev/input/event* may still need a separate helper or udev rule"

# Installs the user unit and reloads systemd. Enable/start separately, e.g.
#   systemctl --user enable --now modore-host.service
install-systemd-user: bootstrap-evdev-access
	install -d "$(SYSTEMD_USER_DIR)"
	install -m644 "$(SYSTEMD_UNIT)" "$(SYSTEMD_USER_DIR)/modore-host.service"
	@echo "systemd: user unit installed"

reload-user-service:
	systemctl --user daemon-reload
	systemctl --user restart modore-host

clean:
	rm -rf "$(BUILD)"
