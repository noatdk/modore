APP_NAME ?= modore
BUNDLE_ID ?= local.modore.host
SIGN_ID   ?= modore Dev
BUILD     ?= build
APP       := $(BUILD)/$(APP_NAME).app
BIN       := $(APP)/Contents/MacOS/modore-host
PLIST     := $(APP)/Contents/Info.plist
# Auto-discover every Swift file under native/macos/, excluding $(BUILD).
# Sources are grouped by concern in App/ Pickup/ Hotkey/ Config/ UI/
# Diagnostics/ — Swift doesn't care about ordering or directories (the
# module compiles all files together and resolves cross-file symbols),
# so adding a new .swift file anywhere under here Just Works on next build.
SRC       := $(shell find . -name '*.swift' -not -path './$(BUILD)/*')

# Cross-platform mozc bridge (cmake project at repo root)
REPO_ROOT       := $(abspath ../..)
BRIDGE_SRC_DIR  := $(REPO_ROOT)/bridge
BRIDGE_BUILD    := $(BRIDGE_SRC_DIR)/build
BRIDGE_LIB      := $(BRIDGE_BUILD)/libmozc_bridge.dylib
BUNDLE_DYLIB    := $(APP)/Contents/Frameworks/libmozc_bridge.dylib
BRIDGE_HEADER   := $(BRIDGE_SRC_DIR)/include/mozc_bridge.h
BRIDGE_HEADERS  := $(wildcard $(BRIDGE_SRC_DIR)/include/*.h)
BRIDGE_SOURCES  := $(wildcard $(BRIDGE_SRC_DIR)/src/*.cc) \
                   $(wildcard $(BRIDGE_SRC_DIR)/src/*.cpp) \
                   $(wildcard $(BRIDGE_SRC_DIR)/src/*.h)

# Lua scripting engine (cmake project at engine/). Built with JIT off for
# now — once we ship the .entitlements with com.apple.security.cs.allow-jit
# we can flip MODORE_SCRIPT_JIT back to ON.
ENGINE_SRC_DIR  := $(REPO_ROOT)/engine
ENGINE_BUILD    := $(REPO_ROOT)/build/engine
ENGINE_LIB      := $(ENGINE_BUILD)/libmodore_script.dylib
BUNDLE_ENGINE   := $(APP)/Contents/Frameworks/libmodore_script.dylib
ENGINE_HEADER   := $(ENGINE_SRC_DIR)/include/modore_script.h
ENGINE_SOURCES  := $(wildcard $(ENGINE_SRC_DIR)/src/*.c) \
                   $(wildcard $(ENGINE_SRC_DIR)/src/*.h) \
                   $(wildcard $(ENGINE_SRC_DIR)/include/*.h)

BRIDGING_HEADER := $(BUILD)/bridging-header.h

.PHONY: all run open clean clean-bridge signing test

# Swift unit-test driver. Self-discovers what to compile:
#
#   TESTABLE_SRC — every native/macos/ source whose header starts with the
#                  `// @testable` marker. The marker is opt-in (not "no
#                  platform-framework imports") because some Foundation-only
#                  files still link C symbols that pull in the bridge dylib
#                  at runtime; the marker is the contract the file's author
#                  signs to say "this compiles standalone with Foundation."
#   TEST_FILES   — every *.swift under test/macos/. Drop a new test in there
#                  and `make test` picks it up; no Makefile edit needed.
#
# The build joins both sets into one swiftc invocation (Swift requires top-
# level helpers and tests to compile as one module since the project doesn't
# ship a SwiftPM manifest).
TESTABLE_SRC := $(shell grep -lE '^// @testable' $(SRC) 2>/dev/null)
TEST_FILES   := $(shell find ../../test/macos -name '*.swift' 2>/dev/null)
TEST_BIN     := $(BUILD)/unit-tests

all: $(APP)

# Idempotent on every build — exits silently if the identity is already
# present, creates it on a fresh machine. Keeping it a prerequisite means
# `make` Just Works from a clean clone.
signing:
	@bash scripts/setup-signing.sh

$(APP): $(BIN) $(PLIST) $(BUNDLE_DYLIB) $(BUNDLE_ENGINE) | signing
	# Re-sign the whole bundle. The dylib must be signed before the executable
	# (codesign verifies inner content), but copying it in via $(BUNDLE_DYLIB)
	# above is a clean-target dep, so by here it's already in place.
	codesign --force --sign "$(SIGN_ID)" --identifier $(BUNDLE_ID).bridge "$(BUNDLE_DYLIB)"
	codesign --force --sign "$(SIGN_ID)" --identifier $(BUNDLE_ID).script "$(BUNDLE_ENGINE)"
	codesign --force --sign "$(SIGN_ID)" --identifier $(BUNDLE_ID) "$(APP)"
	@echo "Built $(APP)"

# --- bridge (cmake) ---------------------------------------------------------
# We re-run cmake every build because cmake itself handles incremental builds
# below it. The Swift target depends on $(BRIDGE_LIB), so this only fires when
# the bridge sources actually change.
#
# Mozc's build scripts call bare `python` (no `3`). We prepend a tiny shim
# directory (bridge/build-tools/) to PATH so those scripts find a working
# interpreter without touching the user's global environment.
BRIDGE_PATH := $(BRIDGE_SRC_DIR)/build-tools:$(PATH)

$(BRIDGE_LIB): $(BRIDGE_SOURCES) $(BRIDGE_HEADERS) $(BRIDGE_SRC_DIR)/CMakeLists.txt
	PATH="$(BRIDGE_PATH)" cmake -S "$(BRIDGE_SRC_DIR)" -B "$(BRIDGE_BUILD)" -G "Unix Makefiles" \
		-DCMAKE_BUILD_TYPE=Release
	PATH="$(BRIDGE_PATH)" cmake --build "$(BRIDGE_BUILD)" -j

# --- scripting engine (cmake) ----------------------------------------------
# LuaJIT must be fetched first: `make fetch-luajit` at repo root. We fail
# loudly with the actionable command rather than auto-clone, so users
# without scripting needs don't pay the clone cost on an unrelated build.
$(ENGINE_LIB): $(ENGINE_SOURCES) $(ENGINE_SRC_DIR)/CMakeLists.txt
	@if [ ! -d "$(REPO_ROOT)/third_party/luajit/.git" ]; then \
	    echo "scripting engine needs LuaJIT — run 'make fetch-luajit' at repo root"; exit 1; \
	fi
	cmake -S "$(ENGINE_SRC_DIR)" -B "$(ENGINE_BUILD)" -G "Unix Makefiles" \
		-DCMAKE_BUILD_TYPE=Release -DMODORE_SCRIPT_JIT=OFF
	cmake --build "$(ENGINE_BUILD)" -j --target modore_script

# --- Swift host -------------------------------------------------------------
# Swift needs a bridging header to pull in the C ABI. We generate a tiny
# wrapper that just includes the real one, so the canonical header stays in
# bridge/include/ and isn't duplicated.
CLS_HEADER      := $(ENGINE_SRC_DIR)/include/mdr_classifier.h

$(BRIDGING_HEADER): $(BRIDGE_HEADER) $(ENGINE_HEADER) $(CLS_HEADER)
	mkdir -p "$(BUILD)"
	@printf '// auto-generated by Makefile\n#include "%s"\n#include "%s"\n#include "%s"\n' \
	    "$(BRIDGE_HEADER)" "$(ENGINE_HEADER)" "$(CLS_HEADER)" > "$@"

$(BIN): $(SRC) $(BRIDGE_LIB) $(ENGINE_LIB) $(BRIDGING_HEADER)
	mkdir -p "$(APP)/Contents/MacOS"
	swiftc -O \
		-framework Carbon -framework Cocoa -framework ApplicationServices \
		-import-objc-header "$(BRIDGING_HEADER)" \
		-Xcc -I"$(BRIDGE_SRC_DIR)/include" \
		-Xcc -I"$(ENGINE_SRC_DIR)/include" \
		-L"$(BRIDGE_BUILD)" -lmozc_bridge \
		-L"$(ENGINE_BUILD)" -lmodore_script \
		-Xlinker -rpath -Xlinker "@executable_path/../Frameworks" \
		-lc++ \
		-o "$@" $(SRC)

# Copy the bridge dylib into the bundle's Frameworks/ dir; the @rpath in the
# executable's load commands points here.
$(BUNDLE_DYLIB): $(BRIDGE_LIB)
	mkdir -p "$(APP)/Contents/Frameworks"
	cp "$(BRIDGE_LIB)" "$(BUNDLE_DYLIB)"

# Same for the scripting engine. install_name is rewritten so the .app
# resolves it via @rpath rather than the build-tree path.
$(BUNDLE_ENGINE): $(ENGINE_LIB)
	mkdir -p "$(APP)/Contents/Frameworks"
	cp "$(ENGINE_LIB)" "$(BUNDLE_ENGINE)"
	install_name_tool -id "@rpath/libmodore_script.dylib" "$(BUNDLE_ENGINE)"

$(PLIST): Info.plist
	mkdir -p "$(APP)/Contents"
	cp Info.plist "$@"

# --- helpers ---------------------------------------------------------------
run: $(APP)
	"$(BIN)"

open: $(APP)
	open "$(APP)"

clean:
	rm -rf $(BUILD)

clean-bridge:
	rm -rf $(BRIDGE_BUILD)

# Compile + run the auto-discovered Swift unit tests. Test sources and
# `// @testable`-marked helpers compile as one Foundation-only module —
# no Cocoa/Carbon/bridge linkage required.
$(TEST_BIN): $(TESTABLE_SRC) $(TEST_FILES)
	@if [ -z "$(TEST_FILES)" ]; then echo "no test sources found under ../../test/macos"; exit 1; fi
	@if [ -z "$(TESTABLE_SRC)" ]; then echo "no '// @testable' marked sources found under native/macos"; exit 1; fi
	mkdir -p "$(BUILD)"
	swiftc -O -o "$@" $(TESTABLE_SRC) $(TEST_FILES)

test: $(TEST_BIN)
	"$(TEST_BIN)"
