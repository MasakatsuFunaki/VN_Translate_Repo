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
    src/ui_text_lookup.cpp
    src/ui_text_lookup.h
    src/chrome_text_logic.cpp
    src/chrome_text_logic.h
)
target_include_directories(translator_logic PUBLIC src)
target_compile_definitions(translator_logic PRIVATE
    _CRT_SECURE_NO_WARNINGS
)

# --------------------------------------------------------------------
# menu_patch_format: pure (OS-independent) "AMK2MENU" container parser,
# MurmurHash2, XOR unmasker, relative-path validator and applied-sentinel
# codec. Shared by the proxy DLL and the auto-patcher unit tests.
# --------------------------------------------------------------------
add_library(menu_patch_format OBJECT
    src/menu_patch_format.cpp
    src/menu_patch_format.h
)
target_include_directories(menu_patch_format PUBLIC src)
target_compile_definitions(menu_patch_format PRIVATE
    _CRT_SECURE_NO_WARNINGS
)

# --------------------------------------------------------------------
# proxy_log: thread-safe diagnostic file logger.
# --------------------------------------------------------------------
add_library(proxy_log OBJECT
    src/log.cpp
    src/log.h
)
target_include_directories(proxy_log PUBLIC src)
target_compile_definitions(proxy_log PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
)

# --------------------------------------------------------------------
# menu_autopatch: Win32 first-launch extractor that reconstructs loose
# <direct>1</direct> override files from <GameDir>\menu_patch.bin.
# --------------------------------------------------------------------
add_library(menu_autopatch OBJECT
    src/menu_autopatch.cpp
    src/menu_autopatch.h
)
target_include_directories(menu_autopatch PUBLIC src)
target_compile_definitions(menu_autopatch PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
)

# --------------------------------------------------------------------
# win32_chrome: import-table hooks that show the English window title
# while startup.xml keeps the Japanese <title> the save folder needs.
# --------------------------------------------------------------------
add_library(win32_chrome OBJECT
    src/win32_chrome.cpp
    src/win32_chrome.h
)
target_include_directories(win32_chrome PUBLIC src)
target_compile_definitions(win32_chrome PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
)

# --------------------------------------------------------------------
# gdi_text_measurer: Pimpl-insulated Win32 GDI text width strategy
# shared between the proxy DLL and GDI-backed unit tests.
# --------------------------------------------------------------------
add_library(gdi_text_measurer OBJECT
    src/gdi_text_measurer.cpp
    src/gdi_text_measurer.h
)
target_include_directories(gdi_text_measurer PUBLIC src)
target_compile_definitions(gdi_text_measurer PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
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
    src/winmm.def
    $<TARGET_OBJECTS:translator_logic>
    $<TARGET_OBJECTS:gdi_text_measurer>
    $<TARGET_OBJECTS:menu_patch_format>
    $<TARGET_OBJECTS:proxy_log>
    $<TARGET_OBJECTS:menu_autopatch>
    $<TARGET_OBJECTS:win32_chrome>
)

target_link_libraries(proxy PRIVATE gdi32 user32)

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
