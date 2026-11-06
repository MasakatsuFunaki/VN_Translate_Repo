cmake_minimum_required(VERSION 3.20)
project(MushiguruiProxy LANGUAGES CXX)

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

# The proxy DLL must be 32-bit (mushigurui_HD10.exe is x86). Tests
# are built for the same architecture so they can reuse exactly the
# same patcher_logic / translator_logic object code that ships in
# the DLL -- catching any arch-specific surprises (pointer widths,
# int32 overflow in rel32 math) in the tests instead of in the field.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "Must build as 32-bit! Ninja takes its architecture from the compiler "
        "environment, so configure through the x86 preset -- its Conan profile "
        "writes the conanvcvars.bat that selects the 32-bit toolchain. "
        "build.py does this for you.")
endif()

option(ENABLE_TESTS "Build the Google Test suite for the logic modules" ON)

# --------------------------------------------------------------------
# patcher_logic: pure (OS-independent) byte-level helpers used by the
# runtime patcher (rel32 encoding, NOP fill, byte-prefix verification,
# LooksLikeAscii predicate). Built as an OBJECT library so both the
# DLL target and the test binary can compile it once and link the
# same object files.
# --------------------------------------------------------------------
add_library(patcher_logic OBJECT
    src/patcher_logic.cpp
    src/patcher_logic.h
)
target_include_directories(patcher_logic PUBLIC src)
target_compile_definitions(patcher_logic PRIVATE
    _CRT_SECURE_NO_WARNINGS
)

# --------------------------------------------------------------------
# translator_logic: pure TSV parsing / escape-expansion used by the
# runtime translator. Same OBJECT-library pattern as patcher_logic so
# the DLL and tests share object files.
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
# Includes all the Windows-specific glue plus both logic OBJECT
# libraries, so the shipped DLL and the gtest binary exercise the
# exact same code.
# --------------------------------------------------------------------
add_library(proxy SHARED
    src/dllmain.cpp
    src/proxy.cpp
    src/proxy.h
    src/patcher.cpp
    src/patcher.h
    src/translator.cpp
    src/translator.h
    src/log.h
    src/winmm.def
    $<TARGET_OBJECTS:patcher_logic>
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
# Install: the DLL only, into its own subfolder of the install tree.
#
# game/ is separate from bin/ because this DLL carries a system DLL's name
# and is 32-bit: a winmm.dll sitting beside the x64 pipeline executables is
# something the loader picks up and then refuses. RUNTIME on its own also
# leaves the import library (winmm.lib) behind, which nothing outside this
# build links against.
# --------------------------------------------------------------------
install(TARGETS proxy RUNTIME DESTINATION game)

# --------------------------------------------------------------------
# Tests: gtest-based suites that exercise every pure function in both
# logic modules. Gated on ENABLE_TESTS because gtest (pulled via
# Conan) is only a dev-time dependency.
# --------------------------------------------------------------------
if(ENABLE_TESTS)
    find_package(GTest CONFIG REQUIRED)
    enable_testing()
    add_subdirectory(tests)
endif()
