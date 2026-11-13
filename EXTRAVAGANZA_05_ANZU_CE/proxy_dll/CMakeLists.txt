cmake_minimum_required(VERSION 3.15)
project(winmm_proxy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/utf-8)
endif()

if(CMAKE_SIZEOF_VOID_P AND NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "This proxy DLL MUST be built as 32-bit (Win32) to match the game executable.")
endif()

option(ENABLE_TESTS "Build the GoogleTest suite (UT)" ON)

add_library(translator_logic OBJECT
    src/translator_logic.cpp
    src/patcher_logic.cpp
    src/menu_autopatch.cpp
    src/menu_autopatch.h
    src/log.cpp
)
target_include_directories(translator_logic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(translator_logic PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

add_library(winmm SHARED
    src/dllmain.cpp
    src/proxy.cpp
    src/patcher.cpp
    src/translator.cpp
    src/winmm.def
    $<TARGET_OBJECTS:translator_logic>
)

target_compile_definitions(winmm PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

target_link_options(winmm PRIVATE
    /DEF:${CMAKE_CURRENT_SOURCE_DIR}/src/winmm.def
)

install(TARGETS winmm
    RUNTIME DESTINATION game
)

if(ENABLE_TESTS)
    find_package(GTest REQUIRED)
    enable_testing()
    add_subdirectory(tests)
endif()
