APP_NAME ?= modore
BUNDLE_ID ?= local.modore.host
SIGN_ID   ?= modore Dev
BUILD     ?= build
MODORE_ENABLE_SCRIPTING ?= 0
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
# Lean shell-native client, built alongside the bridge by the same cmake run.
SHELL_CLIENT     := $(BRIDGE_BUILD)/modore-shell
BUNDLE_SHELL     := $(APP)/Contents/MacOS/modore-shell
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
CLASSIFIER_MODEL_NAME := classifier.mdl
CLASSIFIER_MODEL := $(ENGINE_SRC_DIR)/models/$(CLASSIFIER_MODEL_NAME)
CLASSIFIER_DICT  := $(ENGINE_SRC_DIR)/models/english_dict.txt
BUNDLE_RESOURCES := $(APP)/Contents/Resources
BUNDLE_MODEL     := $(BUNDLE_RESOURCES)/$(CLASSIFIER_MODEL_NAME)
BUNDLE_DICT      := $(BUNDLE_RESOURCES)/english_dict.txt
SCRIPTING_CMAKE_FLAG := -DMODORE_SCRIPT_ENABLE_LUA=$(if $(filter 1,$(MODORE_ENABLE_SCRIPTING)),ON,OFF)

BRIDGING_HEADER      := $(BUILD)/bridging-header.h
TEST_BRIDGING_HEADER := $(BUILD)/test-bridging-header.h

.PHONY: all run open clean clean-bridge signing test

# Swift unit-test driver. Self-discovers what to compile:
#
#   TESTABLE_SRC — every native/macos/ source whose header starts with the
#                  `// @testable` marker. The marker is opt-in (not "no
#                  platform-framework imports") because some Foundation-only
#                  files still link C symbols that pull in engine dylibs at
#                  runtime; the marker is the contract the file's author
#                  signs to say "this compiles without platform frameworks."
#   TEST_FILES   — every *.swift under test/macos/. Drop a new test in there
#                  and `make test` picks it up; no Makefile edit needed.
#
# The build joins both sets into one swiftc invocation (Swift requires top-
# level helpers and tests to compile as one module since the project doesn't
# ship a SwiftPM manifest).
TESTABLE_SRC := $(shell grep -lE '^// @testable' $(SRC) 2>/dev/null)
TEST_FILES   := $(shell find ../../test/macos -name '*.swift' 2>/dev/null)
TEST_BIN     := $(BUILD)/tests/unit-tests

all: $(APP)

# Idempotent on every build — exits silently if the identity is already
# present, creates it on a fresh machine. Keeping it a prerequisite means
# `make` Just Works from a clean clone.
signing:
	@bash scripts/setup-signing.sh

$(APP): $(BIN) $(PLIST) $(BUNDLE_DYLIB) $(BUNDLE_ENGINE) $(BUNDLE_SHELL) $(BUNDLE_MODEL) $(BUNDLE_DICT) | signing
	rm -f "$(APP)/Contents/MacOS/unit-tests"
	# Re-sign the whole bundle. Inner Mach-O (dylibs + the modore-shell client)
	# must be signed before the bundle seal; copying them in above are clean-
	# target deps, so by here they're already in place.
	codesign --force --sign "$(SIGN_ID)" --identifier $(BUNDLE_ID).bridge "$(BUNDLE_DYLIB)"
	codesign --force --sign "$(SIGN_ID)" --identifier $(BUNDLE_ID).script "$(BUNDLE_ENGINE)"
	codesign --force --sign "$(SIGN_ID)" --identifier $(BUNDLE_ID).shell "$(BUNDLE_SHELL)"
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
# Default host builds keep classifier/text helpers and omit LuaJIT-backed
# scripting hooks. Set MODORE_ENABLE_SCRIPTING=1 to opt back in.
$(ENGINE_LIB): $(ENGINE_SOURCES) $(ENGINE_SRC_DIR)/CMakeLists.txt
	@if [ "$(MODORE_ENABLE_SCRIPTING)" = "1" ] && [ ! -d "$(REPO_ROOT)/third_party/luajit/.git" ]; then \
	    echo "scripting engine needs LuaJIT — run 'make fetch-luajit' at repo root"; exit 1; \
	fi
	cmake -S "$(ENGINE_SRC_DIR)" -B "$(ENGINE_BUILD)" -G "Unix Makefiles" \
		-DCMAKE_BUILD_TYPE=Release -DMODORE_SCRIPT_JIT=OFF $(SCRIPTING_CMAKE_FLAG)
	cmake --build "$(ENGINE_BUILD)" -j --target modore_script

# --- Swift host -------------------------------------------------------------
# Swift needs a bridging header to pull in the C ABI. We generate a tiny
# wrapper that just includes the real one, so the canonical header stays in
# bridge/include/ and isn't duplicated.
CLS_HEADER      := $(ENGINE_SRC_DIR)/include/mdr_classifier.h
SHADOW_HEADER   := $(ENGINE_SRC_DIR)/include/shadow_buffer.h

$(BRIDGING_HEADER): $(BRIDGE_HEADER) $(ENGINE_HEADER) $(CLS_HEADER) $(SHADOW_HEADER)
	mkdir -p "$(BUILD)"
	@printf '// auto-generated by Makefile\n#include "%s"\n#include "%s"\n#include "%s"\n#include "%s"\n' \
	    "$(BRIDGE_HEADER)" "$(ENGINE_HEADER)" "$(CLS_HEADER)" "$(SHADOW_HEADER)" > "$@"

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

# The cmake run behind $(BRIDGE_LIB) builds modore-shell too (it's a default
# target), so depend on the dylib to guarantee it exists, then copy it in.
$(BUNDLE_SHELL): $(BRIDGE_LIB)
	mkdir -p "$(APP)/Contents/MacOS"
	cp "$(SHELL_CLIENT)" "$(BUNDLE_SHELL)"

# Same for the scripting engine. install_name is rewritten so the .app
# resolves it via @rpath rather than the build-tree path.
$(BUNDLE_ENGINE): $(ENGINE_LIB)
	mkdir -p "$(APP)/Contents/Frameworks"
	cp "$(ENGINE_LIB)" "$(BUNDLE_ENGINE)"
	install_name_tool -id "@rpath/libmodore_script.dylib" "$(BUNDLE_ENGINE)"

$(PLIST): Info.plist
	mkdir -p "$(APP)/Contents"
	cp Info.plist "$@"

$(BUNDLE_MODEL): $(CLASSIFIER_MODEL)
	mkdir -p "$(BUNDLE_RESOURCES)"
	cp "$(CLASSIFIER_MODEL)" "$@"

$(BUNDLE_DICT): $(CLASSIFIER_DICT)
	mkdir -p "$(BUNDLE_RESOURCES)"
	cp "$(CLASSIFIER_DICT)" "$@"

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
# `// @testable`-marked helpers compile as one Foundation-only module,
# with the scripting engine linked for shared portable text policy.
$(TEST_BRIDGING_HEADER): $(ENGINE_HEADER) $(SHADOW_HEADER)
	mkdir -p "$(BUILD)"
	@printf '// auto-generated by Makefile\n#include "%s"\n#include "%s"\n' \
	    "$(ENGINE_HEADER)" "$(SHADOW_HEADER)" > "$@"

$(TEST_BIN): $(TESTABLE_SRC) $(TEST_FILES) $(TEST_BRIDGING_HEADER) $(ENGINE_LIB)
	@if [ -z "$(TEST_FILES)" ]; then echo "no test sources found under ../../test/macos"; exit 1; fi
	@if [ -z "$(TESTABLE_SRC)" ]; then echo "no '// @testable' marked sources found under native/macos"; exit 1; fi
	mkdir -p "$(dir $@)"
	swiftc -O \
		-import-objc-header "$(TEST_BRIDGING_HEADER)" \
		-Xcc -I"$(ENGINE_SRC_DIR)/include" \
		-L"$(ENGINE_BUILD)" -lmodore_script \
		-Xlinker -rpath -Xlinker "$(ENGINE_BUILD)" \
		-o "$@" $(TESTABLE_SRC) $(TEST_FILES)

test: $(TEST_BIN)
	"$(TEST_BIN)"
