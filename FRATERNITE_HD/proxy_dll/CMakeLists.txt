cmake_minimum_required(VERSION 3.20)
project(FraterniteHDProxy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Force MSVC to treat source files as UTF-8. Without this, MSVC on
# Japanese-locale Windows defaults to interpreting source bytes as
# CP932 (Shift-JIS), which mangles the UTF-8 Japanese characters we
# use in comments -- sometimes even swallowing newlines and causing
# real C2065 compile errors on the next line. This is the standard
# fix every MSVC project with non-ASCII source needs.
if(MSVC)
    add_compile_options(/utf-8)
endif()

# The proxy DLL must be 32-bit (fraternite_hd.exe is x86). Tests are
# built for the same architecture so they can reuse exactly the same
# translator_logic object code that ships in the DLL -- catching any
# arch-specific surprises in the tests instead of in the field.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "Must build as 32-bit! Ninja takes its architecture from the compiler "
        "environment, so configure through the x86 preset -- its Conan profile "
        "writes the conanvcvars.bat that selects the 32-bit toolchain. "
        "build.py does this for you.")
endif()

option(ENABLE_TESTS "Build the Google Test suite for translator_logic" ON)

# --------------------------------------------------------------------
# translator_logic: pure (OS-independent) translation logic. Built as
# an OBJECT library so both the DLL target and the test binary can
# compile it once and link the same object files.
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
# proxy DLL: the actual winmm.dll that gets dropped next to the game.
# Includes all the Windows-specific glue plus the translator_logic
# object files.
# --------------------------------------------------------------------
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
# Architecture: IAT-hook GDI32!TextOutA. The engine renders dialog one
# CP932 glyph at a time via TextOutA into a memory DC; on each call we
# read the engine's state struct (buf38/buf3c/opcount), build a per-
# message render plan with one ASCII chunk per glyph slot, and substitute
# the chunk for the current slot using a narrower font.

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

# --------------------------------------------------------------------
# Install: the DLL, into game/ rather than beside the x64 executables.
# It carries a system DLL's name and it is 32-bit, so a copy next to the
# pipeline exes is a winmm.dll the loader picks up and then refuses.
# RUNTIME alone also leaves the import .lib behind, which nothing needs.
# --------------------------------------------------------------------
install(TARGETS proxy RUNTIME DESTINATION game)

# --------------------------------------------------------------------
# Tests: gtest-based suites that exercise every pure function in
# translator_logic plus a bank of regression cases against the real
# game TSV. Gated on ENABLE_TESTS because gtest (pulled via Conan) is
# only a dev-time dependency.
# --------------------------------------------------------------------
if(ENABLE_TESTS)
    find_package(GTest CONFIG REQUIRED)
    enable_testing()
    add_subdirectory(tests)
endif()
