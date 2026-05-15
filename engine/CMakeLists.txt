# engine — Lua scripting engine for modore.
#
# Builds libmodore_script.{dylib,so,dll}: a shared library that embeds
# LuaJIT 2.1 statically and exposes a flat C ABI (modore_script_*) to
# host frontends. macOS and Linux hosts dlopen this lib; Windows will
# later. No per-host VM copy.
#
# LuaJIT source is NOT a submodule. It's fetched lazily by the top-level
# `make fetch-luajit` target into third_party/luajit/. This file just
# wraps that pre-fetched tree as an IMPORTED static archive.

cmake_minimum_required(VERSION 3.22)
project(modore_script C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(MODORE_SCRIPT_ENABLE_LUA "Build Lua-backed scripting hooks" ON)
option(MODORE_SCRIPT_JIT "Enable LuaJIT JIT compiler (off = interpreter only)" ON)
option(MODORE_SCRIPT_BUILD_TESTS "Build smoke / unit tests" ON)

# Hide everything by default; export only what we mark with MODORE_SCRIPT_EXPORT.
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

if(MODORE_SCRIPT_ENABLE_LUA)
    # ---- LuaJIT (vendored under third_party/luajit, fetched by make target) ----

    set(LUAJIT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/luajit")
    if(NOT EXISTS "${LUAJIT_DIR}/Makefile")
        message(FATAL_ERROR
            "third_party/luajit/ is missing.\n"
            "Run from repo root:\n"
            "  make fetch-luajit")
    endif()

    # LuaJIT ships a plain Makefile; ExternalProject wraps it.
    include(ExternalProject)

    # Apple Silicon: pin deployment target so LuaJIT picks the right SDK.
    if(APPLE)
        if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
            set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "" FORCE)
        endif()
        set(_LUAJIT_MACOSX_DEPLOYMENT_TARGET "MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
    else()
        set(_LUAJIT_MACOSX_DEPLOYMENT_TARGET "")
    endif()

    # JIT toggle propagates as XCFLAGS.
    if(MODORE_SCRIPT_JIT)
        set(_LUAJIT_XCFLAGS "")
    else()
        set(_LUAJIT_XCFLAGS "XCFLAGS=-DLUAJIT_DISABLE_JIT")
    endif()

    set(LUAJIT_STATIC_LIB "${LUAJIT_DIR}/src/libluajit.a")

    ExternalProject_Add(luajit_build
        SOURCE_DIR        "${LUAJIT_DIR}"
        CONFIGURE_COMMAND ""
        BUILD_IN_SOURCE   1
        BUILD_COMMAND     make -C <SOURCE_DIR> amalg
                              BUILDMODE=static
                              ${_LUAJIT_MACOSX_DEPLOYMENT_TARGET}
                              ${_LUAJIT_XCFLAGS}
        INSTALL_COMMAND   ""
        BUILD_BYPRODUCTS  "${LUAJIT_STATIC_LIB}"
    )

    add_library(luajit STATIC IMPORTED GLOBAL)
    set_target_properties(luajit PROPERTIES
        IMPORTED_LOCATION             "${LUAJIT_STATIC_LIB}"
        INTERFACE_INCLUDE_DIRECTORIES "${LUAJIT_DIR}/src"
    )
    add_dependencies(luajit luajit_build)
endif()

# ---- libmodore_script ------------------------------------------------------

add_library(modore_script SHARED
    src/classifier.c
    src/pickup_core.c
)
if(MODORE_SCRIPT_ENABLE_LUA)
    target_sources(modore_script PRIVATE
        src/engine.c
        src/sandbox.c
        src/marshal.c
        src/reload.c
    )
else()
    target_sources(modore_script PRIVATE src/engine_stub.c)
    target_compile_definitions(modore_script PRIVATE MDR_SCRIPTING_DISABLED)
endif()
target_include_directories(modore_script
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
if(MODORE_SCRIPT_ENABLE_LUA)
    target_link_libraries(modore_script PRIVATE luajit)
endif()
set_target_properties(modore_script PROPERTIES
    OUTPUT_NAME   modore_script
    C_VISIBILITY_PRESET hidden
)
target_compile_definitions(modore_script PRIVATE MDR_BUILDING)

# macOS: needs libm for exp() in classifier.c. LuaJIT needs libm/dl on Linux.
if(APPLE)
    target_link_libraries(modore_script PRIVATE m)
elseif(UNIX)
    target_link_libraries(modore_script PRIVATE m)
    if(MODORE_SCRIPT_ENABLE_LUA)
        target_link_libraries(modore_script PRIVATE dl)
    endif()
endif()

# Note: LuaJIT's classic -pagezero_size / -image_base trampoline hack is
# x86_64-only and applies to the host executable, not a shared lib. v2.1
# defaults to GC64 mode on arm64, which removes that constraint entirely.
# If a future x86_64 host needs it, it goes on the executable target
# (Swift host / linux host), not here.

# LuaJIT's static archive was built with default visibility; without an
# explicit export filter, every `lua_*` symbol would leak from our .dylib.
# Whitelist only modore_script_* symbols.
if(MODORE_SCRIPT_ENABLE_LUA AND APPLE)
    target_link_options(modore_script PRIVATE
        "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/exports/modore_script.symbols"
    )
elseif(MODORE_SCRIPT_ENABLE_LUA AND UNIX)
    target_link_options(modore_script PRIVATE
        "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports/modore_script.version"
        "-Wl,--exclude-libs,ALL"
    )
endif()

# ---- Tests -----------------------------------------------------------------

if(MODORE_SCRIPT_BUILD_TESTS)
    enable_testing()
    if(MODORE_SCRIPT_ENABLE_LUA)
        foreach(t smoke test_pickup test_pickup_core test_replacement test_route test_candidates test_acquire test_sandbox test_engine test_text)
            add_executable(${t} tests/${t}.c)
            target_link_libraries(${t} PRIVATE modore_script)
            target_include_directories(${t} PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}/tests)
            add_test(NAME ${t} COMMAND ${t})
        endforeach()
    endif()
    # Classifier test links directly against the shared lib (includes classifier.c)
    add_executable(test_classifier tests/test_classifier.c)
    target_link_libraries(test_classifier PRIVATE modore_script)
    target_include_directories(test_classifier PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME test_classifier COMMAND test_classifier)
endif()
