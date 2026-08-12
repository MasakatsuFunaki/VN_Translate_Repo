cmake_minimum_required(VERSION 3.20)
project(MatsuroProxy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Force 32-bit build (game is x86)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "Must build as 32-bit! Ninja takes its architecture from the compiler "
        "environment, so configure through the x86 preset -- its Conan profile "
        "writes the conanvcvars.bat that selects the 32-bit toolchain. "
        "build.py does this for you.")
endif()

add_library(proxy SHARED
    src/dllmain.cpp
    src/proxy.cpp
    src/proxy.h
    src/patcher.cpp
    src/patcher.h
    src/translator.cpp
    src/translator.h
    src/translator.cpp
    src/translator.h
    src/log.h
    src/winmm.def
)

target_compile_definitions(proxy PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

# Output directly named winmm.dll
set_target_properties(proxy PROPERTIES
    OUTPUT_NAME "winmm"
    PREFIX ""
    SUFFIX ".dll"
)

# --------------------------------------------------------------------
# Install: the hook DLL, into its own folder.
#
# game/ is separate from bin/ on purpose.  This DLL carries the name of a
# system DLL and it is 32-bit while the pipeline executables are 64-bit; a
# winmm.dll sitting beside an x64 executable is a DLL the loader will pick up
# and then refuse to load.  RUNTIME alone also excludes the import .lib.
# --------------------------------------------------------------------
install(TARGETS proxy RUNTIME DESTINATION game)

option(ENABLE_TESTS "Build the Google Test suite" ON)
if(ENABLE_TESTS)
    find_package(GTest CONFIG REQUIRED)
    enable_testing()
    add_subdirectory(tests)
endif()
