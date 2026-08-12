cmake_minimum_required(VERSION 3.20)
project(ShingakkouProxy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Force MSVC to treat source files as UTF-8. Without this, MSVC on
# Japanese-locale Windows defaults to interpreting source bytes as
# CP932 (Shift-JIS), which mangles the UTF-8 Japanese characters we
# use in comments -- sometimes even swallowing newlines and causing
# real C2065 compile errors on the next line.
if(MSVC)
    add_compile_options(/utf-8)
endif()

# Force 32-bit build (game is x86). Tests are built for the same
# architecture so they reuse exactly the same patcher_logic /
# translator_logic object code that ships in the DLL.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "Must build as 32-bit! Ninja takes its architecture from the compiler "
        "environment, so configure through the x86 preset -- its Conan profile "
        "writes the conanvcvars.bat that selects the 32-bit toolchain. "
        "build.py does this for you.")
endif()

option(ENABLE_TESTS "Build the Google Test suite for the logic modules" ON)

# --------------------------------------------------------------------
# patcher_logic: pure byte-level helpers used by proportionalizer.cpp
# (EXE patches) AND translator.cpp (decrypt hook trampoline).
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
# translator_logic: pure TSV parsing ({TAB}/{CR}/{LF} tokens),
# UTF-8 BOM detection, big-endian 16/24-bit pack/unpack, and the
# cumulative-delta offset fixup table used by PatchDecryptedData.
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
# --------------------------------------------------------------------
add_library(proxy SHARED
    src/dllmain.cpp
    src/proxy.cpp
    src/proxy.h
    src/iat_hook.cpp
    src/iat_hook.h
    src/proportionalizer.cpp
    src/proportionalizer.h
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

target_link_libraries(proxy PRIVATE gdi32)
target_include_directories(proxy PRIVATE src)

# Output directly named winmm.dll
set_target_properties(proxy PROPERTIES
    OUTPUT_NAME "winmm"
    PREFIX ""
    SUFFIX ".dll"
)

# Statically link the CRT (/MT) so winmm.dll has NO external runtime
# dependencies. As a sideload proxy it must load during the game's
# earliest static-import resolution; depending on the dynamic VC runtime
# (MSVCP140/VCRUNTIME140) can make that early load fail, after which
# Windows silently falls back to System32 winmm and translation never
# activates. Scoped to the DLL + its object libs so the gtest-linked
# test targets keep the default (Conan) runtime.
if(MSVC)
    set_property(TARGET proxy patcher_logic translator_logic
        PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# --------------------------------------------------------------------
# Install: winmm.dll and nothing else, into game/ rather than bin/.
#
# It carries a system DLL's name and it is 32-bit, so putting it beside the
# x64 pipeline executables would give the loader something it picks up and
# then refuses.  RUNTIME alone also leaves the import .lib behind, which is a
# link-time artifact no user needs.  The logic object libraries and the three
# test tiers stay in the build tree.
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
