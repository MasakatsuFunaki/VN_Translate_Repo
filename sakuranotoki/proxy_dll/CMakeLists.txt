cmake_minimum_required(VERSION 3.20)
project(sakuranotoki_version_proxy CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# We replace the game's version.dll with our proxy. version.dll is in
# sakuranotoki.exe's IAT (verified via dumpbin) and is NOT in the
# Windows KnownDLLs list, so DLL search-order resolves the EXE folder
# before System32 -- our proxy gets loaded first, then forwards every
# export to the renamed system version (version_orig.dll, our own copy
# of System32\version.dll alongside the proxy).
#
# An earlier scaffold tried iarsys.dll (the file present in the game
# folder), but sakuranotoki.exe's import table does NOT actually
# reference iarsys.dll -- the launcher worked fine with that DLL absent
# -- so the proxy was never mapped. version.dll-as-proxy is the
# corrected injection vector.
add_library(version_proxy SHARED
    src/dllmain.cpp
    src/proxy.cpp
    src/memory_scanner.cpp
    src/iat_hook.cpp
    src/iat_translator.cpp
    src/export_hook.cpp
    src/text_finder.cpp
    src/function_hook.cpp
    src/page_guard.cpp
    src/engine_scout.cpp
    src/translator.cpp
    src/translator_logic.cpp
    src/log.cpp
)

set_target_properties(version_proxy PROPERTIES
    PREFIX ""
    OUTPUT_NAME "version"
    SUFFIX ".dll"
)

target_compile_definitions(version_proxy PRIVATE
    _WIN32_WINNT=0x0601
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
    UNICODE _UNICODE
)

if (MSVC)
    target_compile_options(version_proxy PRIVATE /W3 /MT /GS- /Gy /GR- /utf-8)
    # Ensure 32-bit build (Sakura no Toki engine is x86).  Checked by pointer
    # size rather than CMAKE_GENERATOR_PLATFORM, which only the Visual Studio
    # generator sets and which is therefore always empty under Ninja.
    if (NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
        message(WARNING
            "Building 64-bit, but the target engine is x86. Ninja takes its "
            "architecture from the compiler environment, so configure through "
            "the x86 preset -- its Conan profile writes the conanvcvars.bat "
            "that selects the 32-bit toolchain. build.py does this for you.")
    endif()
endif()

# Forward exports via .def file. proxy.cpp implements naked-jmp stubs
# (proxy_GetFileVersionInfoA etc.) that jump through cached function
# pointers resolved from System32\version.dll at DllMain ATTACH.
# The .def maps each public name -> proxy_<name> stub. Same pattern
# FAVORITE's winmm.dll uses; eliminates the need for a sibling
# version_orig.dll alongside the proxy.
set_target_properties(version_proxy PROPERTIES
    LINK_FLAGS "/DEF:\"${CMAKE_CURRENT_SOURCE_DIR}/src/version.def\""
)

# Install: game/ is what gets copied into the game folder, and it is kept
# apart from the pipeline executables on purpose.  This DLL carries the name
# of a system DLL and it is 32-bit; a version.dll sitting beside an x64
# executable is a DLL the loader picks up and then refuses to load.  RUNTIME
# alone also leaves the import library behind, which nothing outside this
# build links against.
install(TARGETS version_proxy RUNTIME DESTINATION game)

# Optional: gtest harness (tests/SYSTEM uses GTest; UT/CT only kick in
# once translator_logic is also carved out as an OBJECT lib).
if (BUILD_TESTING)
    find_package(GTest CONFIG REQUIRED)
    enable_testing()
    add_subdirectory(tests)
endif()
