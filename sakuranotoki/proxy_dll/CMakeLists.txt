cmake_minimum_required(VERSION 3.20)
project(sakuranotoki_version_proxy CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8)
endif()

# We replace the game's version.dll with our proxy. version.dll is in
# sakuranotoki.exe's IAT (verified via dumpbin) and is NOT in the
# Windows KnownDLLs list, so DLL search-order resolves the EXE folder
# before System32 -- our proxy gets loaded first, then forwards every
# export to the system version.dll in System32.
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
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

target_compile_definitions(version_proxy PRIVATE
    _WIN32_WINNT=0x0601
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
    UNICODE _UNICODE
)

if (MSVC)
    target_compile_options(version_proxy PRIVATE /W3 /GS- /Gy /GR-)
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
set_target_properties(version_proxy PROPERTIES
    LINK_FLAGS "/DEF:\"${CMAKE_CURRENT_SOURCE_DIR}/src/version.def\""
)

# Install: game/ is what gets copied into the game folder, and it is kept
# apart from the pipeline executables on purpose.
install(TARGETS version_proxy RUNTIME DESTINATION game)

if (BUILD_TESTING)
    find_package(GTest CONFIG REQUIRED)
    enable_testing()
    add_subdirectory(tests)
endif()
