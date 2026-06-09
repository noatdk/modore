BUILD ?= build
CONFIG ?= Release

.PHONY: all build run clean

all: build

build:
	@cmake -S . -B "$(BUILD)" -G "Visual Studio 17 2022" -A x64 -T ClangCL
	@cmake --build "$(BUILD)" --config "$(CONFIG)"

run: build
	@"$(BUILD)/$(CONFIG)/modore.exe"

clean:
	@cmake --build "$(BUILD)" --config "$(CONFIG)" --target clean
