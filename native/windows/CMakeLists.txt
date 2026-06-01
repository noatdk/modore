cmake_minimum_required(VERSION 3.22)
project(modore_host_windows LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(modore
    app.cpp
    main.cpp
    config.cpp
    log.cpp
    pickup.cpp
    textio.cpp
)

target_include_directories(modore PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
    ${CMAKE_CURRENT_SOURCE_DIR}/../../bridge/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../engine/include
)

target_compile_definitions(modore PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_link_libraries(modore PRIVATE
    user32
    shell32
    ole32
    oleaut32
    comdlg32
    advapi32
    uiautomationcore
)

if (MINGW)
    target_link_options(modore PRIVATE -municode)
endif()
