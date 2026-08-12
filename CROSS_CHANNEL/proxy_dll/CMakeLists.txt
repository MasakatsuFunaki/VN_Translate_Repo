cmake_minimum_required(VERSION 3.20)
project(CrossChannelProxy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# /utf-8 for any non-ASCII source. MSVC otherwise reinterprets the
# bytes as the system codepage, which mangles UTF-8 string literals
# AND comments on Japanese-locale Windows.
if(MSVC)
    add_compile_options(/utf-8)
endif()

# cc.exe is x86 (PE32, machine 0x14c). The proxy DLL must match;
# tests build for the same arch so they link the exact same
# translator_logic object code that ships in the DLL.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "Must build as 32-bit! Ninja takes its architecture from the compiler "
        "environment, so configure through the x86 preset -- its Conan profile "
        "writes the conanvcvars.bat that selects the 32-bit toolchain. "
        "build.py does this for you.")
endif()

option(ENABLE_TESTS "Build the Google Test suite for translator_logic" ON)

# ── translator_logic: pure (OS-independent) TSV parser + CP932 helpers.
# Built as an OBJECT library so both the DLL and the test binary share
# the exact same compiled object code — what tests verify is what ships.
add_library(translator_logic OBJECT
    src/translator_logic.cpp
    src/translator_logic.h
)
target_include_directories(translator_logic PUBLIC src)
target_compile_definitions(translator_logic PRIVATE
    _CRT_SECURE_NO_WARNINGS
)

# ── proxy DLL: the actual xinput1_3.dll dropped next to cc.exe.
# Forwards the 8 named XInput exports (cc.exe imports ordinal #2 only,
# but other code that piggybacks on XInput keeps working through us)
# and installs IAT hooks on GDI32 text-rendering APIs in the host EXE.
add_library(proxy SHARED
    src/dllmain.cpp
    src/proxy.cpp
    src/proxy.h
    src/iat_hook.cpp
    src/iat_hook.h
    src/translator.cpp
    src/translator.h
    src/log.h
    src/xinput1_3.def
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
    OUTPUT_NAME "xinput1_3"
    PREFIX ""
    SUFFIX ".dll"
)

target_link_libraries(proxy PRIVATE gdi32 user32 shlwapi)
target_include_directories(proxy PRIVATE src)

# ── Install: the DLL, into its own folder.
# game/ is separate from bin/ on purpose. This DLL carries the name of a
# system DLL and it is 32-bit, while the pipeline executables are 64-bit; an
# xinput1_3.dll sitting beside them is a DLL the loader would pick up and then
# refuse to load. RUNTIME alone also excludes the import .lib, which is a
# link-time artefact and not something a player needs.
install(TARGETS proxy RUNTIME DESTINATION game)

if(ENABLE_TESTS)
    find_package(GTest CONFIG REQUIRED)
    enable_testing()
    # All tests live under tests/, organized into UT/CT/SYSTEM tiers.
    # Each tier creates one executable per file (foo_ut.cpp → foo_ut.exe).
    # ctest -L <tier> runs only that tier (UT, CT, or SYSTEM).
    add_subdirectory(tests)
endif()
