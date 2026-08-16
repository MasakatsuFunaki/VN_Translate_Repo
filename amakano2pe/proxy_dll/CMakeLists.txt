cmake_minimum_required(VERSION 3.20)
project(Amakano2PEProxy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Force MSVC to treat source files as UTF-8. Without this, MSVC on
# Japanese-locale Windows defaults to interpreting source bytes as
# CP932 (Shift-JIS), which mangles UTF-8 Japanese characters in
# comments -- sometimes even swallowing newlines and causing real
# C2065 compile errors on the next line. This is the standard fix
# every MSVC project with non-ASCII source needs.
if(MSVC)
    add_compile_options(/utf-8)
endif()

# Force 32-bit build (cs2.exe is x86). Tests are built for the same
# arch so they link the exact same translator_logic object code that
# ships in the DLL.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "Must build as 32-bit! Ninja takes its architecture from the compiler "
        "environment, so configure through the x86 preset -- its Conan profile "
        "writes the conanvcvars.bat that selects the 32-bit toolchain. "
        "build.py does this for you.")
endif()

option(ENABLE_TESTS "Build the Google Test suite for translator_logic" ON)

# --------------------------------------------------------------------
# translator_logic: pure (OS-independent) translation logic, built
# as an OBJECT library so both the DLL and the test binary compile
# it once and share the resulting object files.
# --------------------------------------------------------------------
add_library(translator_logic OBJECT
    src/translator_logic.cpp
    src/translator_logic.h
)
target_include_directories(translator_logic PUBLIC src)
target_compile_definitions(translator_logic PRIVATE
    _CRT_SECURE_NO_WARNINGS
)

# --------------------------------------------------------------------
# proxy DLL: the actual winmm.dll dropped next to cs2.exe.
# --------------------------------------------------------------------
add_library(proxy SHARED
    src/dllmain.cpp
    src/proxy.cpp
    src/proxy.h
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

target_include_directories(proxy PRIVATE src)

# --------------------------------------------------------------------
# Install: the DLL the user copies next to cs2.exe, into its own folder.
#
# game/ is kept apart from the bin/ the pipeline installs into because this
# DLL carries a system DLL's name and is 32-bit: dropped beside the x64
# executables it would be something the loader picks up and then refuses.
# RUNTIME alone also leaves the import .lib behind, which nothing ships.
# --------------------------------------------------------------------
install(TARGETS proxy RUNTIME DESTINATION game)

# --------------------------------------------------------------------
# Tests: gtest suites that exercise every pure function in
# translator_logic plus integration cases against the real deployed
# TSV. Gated on ENABLE_TESTS because gtest (pulled via Conan) is
# only a dev-time dependency.
# --------------------------------------------------------------------
if(ENABLE_TESTS)
    find_package(GTest CONFIG REQUIRED)
    enable_testing()
    add_subdirectory(tests)
endif()
