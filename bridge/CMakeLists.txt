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
project(mozc_bridge CXX)

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
    ${CMAKE_BINARY_DIR}/fcitx5-mozc/data_manager/oss
)

# Mozc's OSS data manager #includes a pre-generated dictionary blob:
#   #include "data_manager/oss/mozc_data.inc"   (~50MB of compiled-in data)
# fcitx5-mozc doesn't ship this; their CI downloads a fresh build from a
# GitHub release. We mirror that.
set(_MOZC_DATA_INC "${CMAKE_BINARY_DIR}/fcitx5-mozc/data_manager/oss/mozc_data.inc")
set(_MOZC_DATA_URL "https://github.com/fcitx-contrib/fcitx5-mozc/releases/download/latest/mozc_data.inc")
if(NOT EXISTS "${_MOZC_DATA_INC}")
    message(STATUS "Downloading mozc_data.inc (~50MB) from ${_MOZC_DATA_URL}")
    file(DOWNLOAD "${_MOZC_DATA_URL}" "${_MOZC_DATA_INC}"
        SHOW_PROGRESS
        STATUS _DL_STATUS
        TLS_VERIFY ON
    )
    list(GET _DL_STATUS 0 _DL_CODE)
    if(NOT _DL_CODE EQUAL 0)
        file(REMOVE "${_MOZC_DATA_INC}")
        message(FATAL_ERROR "Failed to download mozc_data.inc: ${_DL_STATUS}")
    endif()
endif()

add_subdirectory(${FCITX5_MOZC_DIR} ${CMAKE_BINARY_DIR}/fcitx5-mozc EXCLUDE_FROM_ALL)

# Our bridge.
#
# SHARED rather than STATIC: mozc-static + libprotobuf + absl alone pull in
# ~150 transitive static archives. Building as a dylib lets the linker
# resolve all of those *inside* libmozc_bridge.dylib once, so any consumer
# (Swift host, future Windows/Linux frontend, etc.) only has to link
# `-lmozc_bridge` and pick up our flat C ABI.
add_library(mozc_bridge SHARED
    src/mozc_bridge.cc
    src/direct_client.cc
)

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
        ${FCITX5_MOZC_DIR}/mozc/src
        ${FCITX5_MOZC_DIR}/abseil-cpp
        ${FCITX5_MOZC_DIR}/protobuf/src
        # Generated proto headers live in fcitx5-mozc's binary dir.
        ${CMAKE_BINARY_DIR}/fcitx5-mozc
)

# Mozc requires char to be unsigned (uses it as array index).
target_compile_options(mozc_bridge PRIVATE -funsigned-char -Wall -Wextra -Wno-unused-parameter)

# Keep our exported C symbols visible to the consumer (Swift host etc.).
set_target_properties(mozc_bridge PROPERTIES
    C_VISIBILITY_PRESET default
    CXX_VISIBILITY_PRESET default
)

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

target_link_libraries(mozc_bridge PUBLIC
    libprotobuf
    mozc-static
    absl::flags_parse
    absl::log_flags
    ${_absl_targets}
)

# Mozc's CMake build generates protobuf headers and a handful of data files
# via a custom `gen` target. target_link_libraries only adds link-time deps,
# not build-time-order deps, so we have to explicitly require `gen` (and
# `mozc-static`, which itself depends on `gen`) to finish *before* the bridge
# compilation tries to #include the generated headers.
add_dependencies(mozc_bridge gen mozc-static)
