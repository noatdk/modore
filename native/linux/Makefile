# modore — Linux host (X11 + AT-SPI2 + in-process Mozc bridge).

REPO_ROOT := $(abspath ../..)
BRIDGE_SRC := $(REPO_ROOT)/bridge
BRIDGE_BUILD := $(BRIDGE_SRC)/build
BRIDGE_LIB := $(BRIDGE_BUILD)/libmozc_bridge.so

BUILD := build
OUT := $(BUILD)/modore-host
BRIDGE_SO := $(BUILD)/libmozc_bridge.so

CXX ?= g++

export PATH := $(REPO_ROOT)/bridge/build-tools:$(PATH)

PKG_ATSPI := $(shell pkg-config --cflags --libs atspi-2)
PKG_X11 := $(shell pkg-config --cflags --libs x11 xtst)

CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -I$(BRIDGE_SRC)/include -pthread
LDFLAGS := -pthread -Wl,-rpath,\$$ORIGIN
# gobject: g_object_ref/unref (atspi headers use GObject; link needs -lgobject).
PKG_GOBJECT := $(shell pkg-config --libs gobject-2.0)

SRC := main.cpp config.cpp ipc.cpp log.cpp

PREFIX := $(HOME)/.local
INSTALL_BIN := $(PREFIX)/bin/modore-host
SYSTEMD_USER_DIR := $(HOME)/.config/systemd/user
SYSTEMD_UNIT := $(CURDIR)/systemd/user/modore-host.service

.PHONY: all run clean install-user-bin install-systemd-user

all: $(OUT)

# Top-level orchestrator builds the shared bridge (CMake + Mozc).
$(BRIDGE_LIB):
	@$(MAKE) -C "$(REPO_ROOT)" bridge

$(OUT): $(SRC) $(BRIDGE_LIB) Makefile
	mkdir -p "$(BUILD)"
	cp -f "$(BRIDGE_LIB)" "$(BRIDGE_SO)"
	$(CXX) $(CXXFLAGS) -o "$(OUT)" $(SRC) $(LDFLAGS) -L"$(BUILD)" -lmozc_bridge -lstdc++ \
		$(PKG_ATSPI) $(PKG_GOBJECT) $(PKG_X11)

run: $(OUT)
	"$(OUT)"

# Copies modore-host + libmozc_bridge.so into ~/.local (rpath expects the .so beside
# the real binary; ~/.local/bin/modore-host is a symlink).
install-user-bin: $(OUT)
	install -d "$(PREFIX)/lib/modore" "$(PREFIX)/bin"
	install -m755 "$(OUT)" "$(PREFIX)/lib/modore/modore-host"
	install -m755 "$(BRIDGE_SO)" "$(PREFIX)/lib/modore/libmozc_bridge.so"
	ln -sf "$(PREFIX)/lib/modore/modore-host" "$(INSTALL_BIN)"

# Installs the user unit and reloads systemd. Enable/start separately, e.g.
#   systemctl --user enable --now modore-host.service
install-systemd-user: install-user-bin
	install -d "$(SYSTEMD_USER_DIR)"
	install -m644 "$(SYSTEMD_UNIT)" "$(SYSTEMD_USER_DIR)/modore-host.service"
	@echo "systemd: run  systemctl --user daemon-reload  (then enable/start as needed)"

clean:
	rm -rf "$(BUILD)"
