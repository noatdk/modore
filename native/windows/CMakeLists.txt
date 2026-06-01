cmake_minimum_required(VERSION 3.22)
project(modore_host_windows LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(modore-host
    main.cpp
    config.cpp
    log.cpp
)

target_include_directories(modore-host PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
    ${CMAKE_CURRENT_SOURCE_DIR}/../../bridge/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../engine/include
)

target_compile_definitions(modore-host PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(modore-host PRIVATE
    user32
    shell32
    ole32
    comdlg32
    advapi32
)

if (MINGW)
    target_link_options(modore-host PRIVATE -municode)
endif()
