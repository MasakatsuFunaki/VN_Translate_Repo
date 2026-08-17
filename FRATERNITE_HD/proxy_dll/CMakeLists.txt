cmake_minimum_required(VERSION 3.20)
project(FraterniteHDProxy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC defaults to CP932 on JP-locale Windows; /utf-8 prevents compile errors.
if(MSVC)
    add_compile_options(/utf-8)
endif()

# Must be 32-bit (fraternite_hd.exe is x86).
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "Must build as 32-bit! Ninja takes its architecture from the compiler "
        "environment, so configure through the x86 preset -- its Conan profile "
        "writes the conanvcvars.bat that selects the 32-bit toolchain. "
        "build.py does this for you.")
endif()

option(ENABLE_TESTS "Build the Google Test suite for translator_logic" ON)

# translator_logic: OBJECT lib shared by the DLL and the test binary.
add_library(translator_logic OBJECT
    src/translator_logic.cpp
    src/translator_logic.h
)
target_include_directories(translator_logic PUBLIC src)
target_compile_definitions(translator_logic PRIVATE
    _CRT_SECURE_NO_WARNINGS
)

# proxy DLL (winmm.dll): Windows glue + translator_logic objects.
add_library(proxy SHARED
    src/dllmain.cpp
    src/proxy.cpp
    src/proxy.h
    src/iat_hook.cpp
    src/iat_hook.h
    src/translator.cpp
    src/translator.h
    src/overlay_toggle.cpp
    src/overlay_toggle.h
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

target_link_libraries(proxy PRIVATE gdi32 user32)
target_include_directories(proxy PRIVATE src)

# Install into game/, not bin/ -- a 32-bit winmm.dll next to x64 exes breaks.
install(TARGETS proxy RUNTIME DESTINATION game)

# Tests
if(ENABLE_TESTS)
    find_package(GTest CONFIG REQUIRED)
    enable_testing()
    add_subdirectory(tests)
endif()
