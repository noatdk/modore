# bridge — cross-platform C ABI around the Mozc engine.
#
# Pulls in fcitx-contrib/fcitx5-mozc as a CMake subdirectory (with the
# fcitx5 addon disabled) to get the `mozc-static` target — that target
# contains the entire mozc engine: composer, converter, dictionary,
# prediction, rewriter, session, and the OSS data manager.
#
# The first build downloads & compiles ~150MB of source (mozc + abseil-cpp
# + protobuf) and takes ~3–5 minutes. Subsequent builds are incremental and
# only touch what changed.

cmake_minimum_required(VERSION 3.22)
# C is enabled too: the shared text-policy module (engine/src/pickup_core.c)
# compiles into the bridge alongside the C++ sources.
project(mozc_bridge C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Locate fcitx5-mozc (sibling repo via submodule).
set(FCITX5_MOZC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/fcitx5-mozc")
if(NOT EXISTS "${FCITX5_MOZC_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "third_party/fcitx5-mozc not initialized. Run:\n"
        "  git submodule update --init --recursive")
endif()

# Skip building the fcitx5 plugin — we only want the mozc engine.
set(BUILD_MOZC_ADDON OFF CACHE BOOL "Skip the fcitx5 addon (we want engine only)" FORCE)

# fcitx5-mozc's CI applies this patch to the protobuf submodule before building.
# It strips non-C++ generators from libprotoc (we don't need java/csharp/ruby/etc.)
# and removes its dependency on libupb. Without the patch, libprotoc references
# libupb but fcitx5-mozc disables that target, leading to "library 'libupb'
# not found" at link time. See third_party/fcitx5-mozc/.github/workflows/ci.yml.
set(_PROTOBUF_PATCH "${FCITX5_MOZC_DIR}/patches/protobuf.patch")
execute_process(
    COMMAND git apply --directory=protobuf --reverse --check "${_PROTOBUF_PATCH}"
    WORKING_DIRECTORY "${FCITX5_MOZC_DIR}"
    RESULT_VARIABLE _PATCH_ALREADY_APPLIED
    OUTPUT_QUIET ERROR_QUIET
)
if(NOT _PATCH_ALREADY_APPLIED EQUAL 0)
    message(STATUS "Applying fcitx5-mozc's protobuf.patch")
    execute_process(
        COMMAND git apply --directory=protobuf "${_PROTOBUF_PATCH}"
        WORKING_DIRECTORY "${FCITX5_MOZC_DIR}"
        RESULT_VARIABLE _PATCH_RESULT
    )
    if(NOT _PATCH_RESULT EQUAL 0)
        message(FATAL_ERROR
            "Failed to apply fcitx5-mozc's protobuf patch. "
            "Try resetting the protobuf submodule with: "
            "git -C ${FCITX5_MOZC_DIR}/protobuf reset --hard HEAD && "
            "git -C ${FCITX5_MOZC_DIR}/protobuf clean -fdx")
    endif()
endif()

# fcitx5-mozc disables libupb ("No binary for iOS"), but for our patched
# protobuf the libupb path is gone entirely. Leave the default and let the
# patch handle it — explicit OFF works too.
set(protobuf_BUILD_LIBUPB OFF CACHE BOOL "Patched out of libprotoc by fcitx5-mozc" FORCE)

# Pre-create binary-tree subdirectories that mozc's python generator scripts
# write to. fcitx5-mozc declares them via `add_custom_command OUTPUT` but
# doesn't `mkdir -p` them, and the python scripts assume the target dir
# already exists. file(MAKE_DIRECTORY) runs at configure time.
file(MAKE_DIRECTORY
    ${CMAKE_BINARY_DIR}/fcitx5-mozc/base
    ${CMAKE_BINARY_DIR}/fcitx5-mozc/dictionary
)

add_subdirectory(${FCITX5_MOZC_DIR} ${CMAKE_BINARY_DIR}/fcitx5-mozc EXCLUDE_FROM_ALL)

# Our bridge.
#
# SHARED rather than STATIC: mozc-static + libprotobuf + absl alone pull in
# ~150 transitive static archives. Building as a dylib lets the linker
# resolve all of those *inside* libmozc_bridge.dylib once, so any consumer
# (Swift host, future Windows/Linux frontend, etc.) only has to link
# `-lmozc_bridge` and pick up our flat C ABI.
add_library(mozc_bridge SHARED
    src/backend_candidates.cc
    src/backend_flow.cc
    src/backend_oss.cc
    src/mozc_bridge.cc
    src/shell_convert.cc
    src/direct_client.cc
    # Shared romaji/ASCII text policy, compiled directly from the engine tree
    # (with MDR_TEXT_INTERNAL below, so this copy stays hidden) so the
    # shell-native path uses the same mdr_text_* as the hosts instead of a
    # drifting C++ re-implementation. See engine/include/mdr_text.h.
    ${CMAKE_CURRENT_SOURCE_DIR}/../engine/src/pickup_core.c
)

if(WIN32)
    set_target_properties(mozc_bridge PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS ON
    )
endif()

if(APPLE)
    target_sources(mozc_bridge PRIVATE
        src/backend_google_ime_mac.cc
    )
endif()

# atzc conversion backend (opt-in). backend_atzc.cc talks to an atzc::Converter
# from that tree (github.com/noatdk/atzc) — transport-agnostic. Point
# MODORE_ATZC_DIR at a checkout (lazy-cloned by `make fetch-atzc`). Off by
# default, so a normal build needs nothing extra.
option(MODORE_ENABLE_ATZC "Build the atzc conversion backend" OFF)
set(MODORE_ATZC_DIR "" CACHE PATH "Path to an atzc-server checkout (its include/ + src/)")
if(MODORE_ENABLE_ATZC)
    if(NOT MODORE_ATZC_DIR OR NOT EXISTS "${MODORE_ATZC_DIR}/include/atzc/converter.h")
        message(FATAL_ERROR
            "MODORE_ENABLE_ATZC=ON but MODORE_ATZC_DIR does not point at an "
            "atzc-server checkout (expected include/atzc/converter.h). Run "
            "`make fetch-atzc` or pass -DMODORE_ATZC_DIR=<path>.")
    endif()
    # Consumer sources from the atzc tree. On POSIX this is the socket client +
    # the reconnecting MakeConverter; the Windows host will instead pull atzc's
    # in-process engine (Session + the installed-ATOK engine + frontend_windows).
    if(WIN32)
        message(FATAL_ERROR "atzc backend on Windows is not wired yet "
            "(needs the in-process engine sources; see atzc-server).")
    else()
        target_sources(mozc_bridge PRIVATE
            src/backend_atzc.cc
            ${MODORE_ATZC_DIR}/src/client.cpp
            ${MODORE_ATZC_DIR}/src/frontend_posix.cpp
        )
    endif()
    target_include_directories(mozc_bridge PRIVATE ${MODORE_ATZC_DIR}/include)
    target_compile_definitions(mozc_bridge PRIVATE MODORE_ENABLE_ATZC)
    message(STATUS "atzc backend enabled (MODORE_ATZC_DIR=${MODORE_ATZC_DIR})")
endif()

# On macOS, embed an @rpath-relative install name. The Swift host adds
# `-rpath @executable_path/../Frameworks` and copies the dylib into the
# app bundle's Frameworks dir.
set_target_properties(mozc_bridge PROPERTIES
    MACOSX_RPATH ON
    INSTALL_NAME_DIR "@rpath"
)

target_include_directories(mozc_bridge
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        # Shared text-policy module (mdr_text.h + pickup_core.c's utf8_nav.h).
        ${CMAKE_CURRENT_SOURCE_DIR}/../engine/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../engine/src
        ${FCITX5_MOZC_DIR}/mozc/src
        ${FCITX5_MOZC_DIR}/abseil-cpp
        ${FCITX5_MOZC_DIR}/protobuf/src
        # Generated proto headers live in fcitx5-mozc's binary dir.
        ${CMAKE_BINARY_DIR}/fcitx5-mozc
)

# Keep the bridge's compiled-in copy of the mdr_text_* policy hidden, so the
# engine dylib remains the sole exporter when a host links both.
target_compile_definitions(mozc_bridge PRIVATE MDR_TEXT_INTERNAL)

# Mozc requires char to be unsigned (uses it as array index).
if(MSVC)
    target_compile_options(mozc_bridge PRIVATE /J /W3 /wd4100)
else()
    target_compile_options(mozc_bridge PRIVATE -funsigned-char -Wall -Wextra -Wno-unused-parameter)
endif()

# Keep our exported C symbols visible to the consumer (Swift host etc.).
set_target_properties(mozc_bridge PROPERTIES
    C_VISIBILITY_PRESET default
    CXX_VISIBILITY_PRESET default
)

# Lean shell-native client. Speaks the shell-convert socket protocol directly
# and links nothing from Mozc, so the per-keystroke shell widgets spawn a tiny
# binary (~ms) instead of loading the engine dylib just to relay one request.
add_executable(modore_shell src/shell_client_main.cc)
set_target_properties(modore_shell PROPERTIES OUTPUT_NAME modore-shell)
target_compile_features(modore_shell PRIVATE cxx_std_17)
if(MSVC)
    target_compile_options(modore_shell PRIVATE /W3)
else()
    target_compile_options(modore_shell PRIVATE -Wall -Wextra)
endif()

# fcitx5-mozc's `MOZC_TARGET` variable is set in their subdirectory scope and
# doesn't propagate to ours, so we name its members explicitly. mozc-static
# also doesn't declare its absl deps via target_link_libraries — every absl
# ref is private inside mozc-static — so to build a self-contained dylib we
# have to gather every absl_* target abseil defines and link them in too.
# Iteratively walk the abseil-cpp subtree.
set(_dirs_to_visit "${FCITX5_MOZC_DIR}/abseil-cpp")
set(_absl_targets)
while(_dirs_to_visit)
    list(POP_FRONT _dirs_to_visit _current)
    get_directory_property(_t DIRECTORY "${_current}" BUILDSYSTEM_TARGETS)
    get_directory_property(_s DIRECTORY "${_current}" SUBDIRECTORIES)
    foreach(_target IN LISTS _t)
        if(NOT _target MATCHES "^absl_")
            continue()
        endif()
        # Skip testing/benchmark scaffolding — those targets reference deps
        # like `absl::test_instance_tracker` that aren't built when
        # BUILD_TESTING is off, and CMake's deps resolver errors out.
        if(_target MATCHES "_testing$|^absl_test_|_benchmarks$|^absl_random_internal_distribution_test")
            continue()
        endif()
        get_target_property(_type ${_target} TYPE)
        if(NOT _type STREQUAL "STATIC_LIBRARY")
            continue()
        endif()
        list(APPEND _absl_targets ${_target})
    endforeach()
    list(APPEND _dirs_to_visit ${_s})
endwhile()
list(REMOVE_DUPLICATES _absl_targets)
list(LENGTH _absl_targets _absl_count)
message(STATUS "Linking ${_absl_count} absl_* static libs into mozc_bridge")

# GNU ld can leave undefined google::protobuf::* on the final shared library
# unless libprotobuf is pulled in whole. ld64 does not need this.
if(MSVC)
  set(_MOZC_BRIDGE_PROTOBUF_LINK
      libprotobuf
  )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR WIN32)
  set(_MOZC_BRIDGE_PROTOBUF_LINK
      "-Wl,--whole-archive"
      libprotobuf
      "-Wl,--no-whole-archive"
  )
else()
  set(_MOZC_BRIDGE_PROTOBUF_LINK
      libprotobuf
  )
endif()

set(_MOZC_BRIDGE_PLATFORM_LIBS)
if(WIN32)
  list(APPEND _MOZC_BRIDGE_PLATFORM_LIBS crypt32 imm32)
endif()

target_link_libraries(mozc_bridge PUBLIC
    mozc-static
    ${_MOZC_BRIDGE_PROTOBUF_LINK}
    absl::flags_parse
    absl::log_flags
    ${_absl_targets}
    ${_MOZC_BRIDGE_PLATFORM_LIBS}
)

# Mozc's CMake build generates protobuf headers and a handful of data files
# via a custom `gen` target. target_link_libraries only adds link-time deps,
# not build-time-order deps, so we have to explicitly require `gen` (and
# `mozc-static`, which itself depends on `gen`) to finish *before* the bridge
# compilation tries to #include the generated headers.
add_dependencies(mozc_bridge gen mozc-static)
