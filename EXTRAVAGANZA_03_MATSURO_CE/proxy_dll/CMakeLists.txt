cmake_minimum_required(VERSION 3.20)
project(MatsuroProxy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/utf-8)
endif()

# Force 32-bit build (game is x86)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "Must build as 32-bit! Ninja takes its architecture from the compiler "
        "environment, so configure through the x86 preset -- its Conan profile "
        "writes the conanvcvars.bat that selects the 32-bit toolchain. "
        "build.py does this for you.")
endif()

add_library(translator_logic OBJECT
    src/translator_logic.cpp
    src/patcher_logic.cpp
)
target_include_directories(translator_logic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(translator_logic PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

add_library(proxy SHARED
    src/dllmain.cpp
    src/proxy.cpp
    src/proxy.h
    src/patcher.cpp
    src/patcher.h
    src/menu_autopatch.cpp
    src/menu_autopatch.h
    src/translator.cpp
    src/translator.h
    src/log.h
    src/winmm.def
    $<TARGET_OBJECTS:translator_logic>
)

target_compile_definitions(proxy PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
)

set_target_properties(proxy PROPERTIES
    OUTPUT_NAME "winmm"
    PREFIX ""
    SUFFIX ".dll"
)

install(TARGETS proxy RUNTIME DESTINATION game)

option(ENABLE_TESTS "Build the Google Test suite" ON)
if(ENABLE_TESTS)
    find_package(GTest CONFIG REQUIRED)
    enable_testing()
    add_subdirectory(tests)
endif()
