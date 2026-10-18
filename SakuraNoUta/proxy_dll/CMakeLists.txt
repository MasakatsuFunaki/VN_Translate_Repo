cmake_minimum_required(VERSION 3.20)
project(SakuraNoUtaProxy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC defaults to interpreting source bytes as the system codepage
# (CP932 on Japanese Windows), which mangles UTF-8 string literals
# and even comments.  /utf-8 is the standard fix for any non-ASCII
# source.
if(MSVC)
    add_compile_options(/utf-8)
endif()

# BGI.exe is x86 (PE32, machine 0x14c).  The proxy DLL must match;
# tests are built for the same arch so they link the exact same
# translator_logic object code that ships in the DLL.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "Must build as 32-bit! Ninja takes its architecture from the compiler "
        "environment, so configure through the x86 preset -- its Conan profile "
        "writes the conanvcvars.bat that selects the 32-bit toolchain. "
        "build.py does this for you.")
endif()

option(ENABLE_TESTS "Build the Google Test suite for translator_logic" ON)

# --------------------------------------------------------------------
# translator_logic: pure (OS-independent) TSV parser + CP932 helpers.
# Built as an OBJECT library so both the DLL and the test binary
# share the same compiled object.
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
# text_display_abi: the two naked thunks that carry the engine's calling
# convention across the hook.  x86 inline assembly and nothing else, so
# the test binary can link and exercise the very code the DLL ships.
# --------------------------------------------------------------------
add_library(text_display_abi OBJECT
    src/text_display_abi.cpp
    src/text_display_abi.h
)
target_include_directories(text_display_abi PUBLIC src)
target_compile_definitions(text_display_abi PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

# --------------------------------------------------------------------
# proxy DLL: the actual winmm.dll dropped next to BGI.exe.  Forwards
# every winmm export to the real System32 winmm.dll while installing
# an IAT hook on GDI32.TextOutA in the host EXE.
# --------------------------------------------------------------------
add_library(proxy SHARED
    src/dllmain.cpp
    src/proxy.cpp
    src/proxy.h
    src/iat_hook.cpp
    src/iat_hook.h
    src/translator.cpp
    src/translator.h
    src/log.h
    src/winmm.def
    $<TARGET_OBJECTS:translator_logic>
    $<TARGET_OBJECTS:text_display_abi>
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

# --------------------------------------------------------------------
# Install: the DLL only, into its own subfolder of the install tree.
#
# `game/` rather than `bin/` on purpose.  A proxy DLL carries the name of a
# system DLL, and this half is Win32 while the pipeline executables are x64;
# a 32-bit winmm.dll sitting beside a 64-bit executable is a DLL the loader
# can pick up and then refuse to load.  The split also says what the file is
# for: bin/ is what you run, game/ is what you copy into the game folder.
#
# RUNTIME only: the .lib is an import library, a build artifact.
# --------------------------------------------------------------------
install(TARGETS proxy RUNTIME DESTINATION game)

# --------------------------------------------------------------------
# Tests: gtest suites for translator_logic.  Gated on ENABLE_TESTS
# because gtest (pulled via Conan or vcpkg) is a dev-time-only dep.
# --------------------------------------------------------------------
if(ENABLE_TESTS)
    find_package(GTest CONFIG REQUIRED)
    enable_testing()
    add_subdirectory(tests)
endif()
