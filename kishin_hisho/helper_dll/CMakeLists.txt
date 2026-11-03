cmake_minimum_required(VERSION 3.20)
project(AlpharomHelper LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/utf-8 /W3)
endif()

# Both target binaries (helper DLL and launcher) must be 32-bit, because the
# game KishinHisho.exe is 32-bit and the helper DLL is injected into it.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "Must build as 32-bit! Ninja takes its architecture from the compiler "
        "environment, so configure through the x86 preset -- its Conan profile "
        "writes the conanvcvars.bat that selects the 32-bit toolchain. "
        "build.py does this for you.")
endif()

# --------------------------------------------------------------------
# alpharom_helper.dll: injected into KishinHisho.exe before AlphaROM
# stub runs. Installs a Vectored Exception Handler that:
#   1. Catches the stack-thunk PE walker AV (kernel32 alignment-gap read)
#      and redirects ECX to kernel32 base.
#   2. Detects the manually-mapped recognizedisc.dll DRM check, patches
#      fnRecognizedisc to return 1, sets nRecognizedisc=1, and unwinds
#      the in-progress call via EBP frame walking.
# --------------------------------------------------------------------
add_library(alpharom_helper SHARED
    alpharom_helper.cpp
)
target_compile_definitions(alpharom_helper PRIVATE
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)
target_link_libraries(alpharom_helper PRIVATE kernel32 user32)

# --------------------------------------------------------------------
# launch.exe: 32-bit launcher that does CreateProcess(SUSPENDED) +
# inject helper DLL via remote LoadLibrary + ResumeThread.
# --------------------------------------------------------------------
add_executable(launch
    launch.cpp
)
target_compile_definitions(launch PRIVATE
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)
target_link_libraries(launch PRIVATE kernel32)
# /MT so launch.exe doesn't depend on the VC redistributable
set_target_properties(launch PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
)
set_target_properties(alpharom_helper PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
)

# --------------------------------------------------------------------
# Install: game/ is what gets copied into the game folder.
#
# Both binaries are 32-bit, so they are kept away from the x64 executables
# the root project installs into bin/ -- a 32-bit DLL beside a 64-bit
# executable is something the loader picks up and then refuses.
#
# RUNTIME alone: it takes the .dll and the .exe and leaves the helper's
# import library behind, which nothing outside this build links against.
# --------------------------------------------------------------------
install(TARGETS alpharom_helper launch RUNTIME DESTINATION game)

# Deployment to the game folder is `python build.py --deploy` from the game
# root, or `01_patch.exe`, which wraps the copy and reports missing admin
# rights when the destination is under Program Files.
