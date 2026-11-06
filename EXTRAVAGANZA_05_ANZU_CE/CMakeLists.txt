cmake_minimum_required(VERSION 3.23)

# ==============================================================================
# WelcomeToAnzU_CE -- Root Superbuild
# ==============================================================================
# This project has two halves with different target architectures:
#
#   pipeline_cpp/   x64    00_run_all .. 04_patch_menus + GoogleTest
#   proxy_dll/      Win32  winmm.dll (injected into the 32-bit game)
#
# A single CMake project has one CMAKE_SIZEOF_VOID_P, so it cannot compile
# both architectures directly. Instead, this root CMakeLists.txt drives the
# two existing CMakePresets.json files as sub-builds into the single
# WelcomeToAnzU_CE/build/ tree:
#
#   WelcomeToAnzU_CE/build/
#     root/          this superbuild
#     pipeline/      x64 sub-build + Conan generators
#     proxy/         x86 sub-build + Conan generators
#     install/
#       bin/         00_run_all.exe .. 04_patch_menus.exe + runtime DLLs
#       game/        winmm.dll (ready to copy into the game folder)
#
# Usage (after `python build.py` has run `conan install` once):
#   cmake --preset default
#   cmake --build --preset default
#   ctest --preset default
#   cmake --install build/root
# ==============================================================================

project(WelcomeToAnzUCE LANGUAGES NONE)

option(ENABLE_TESTS "Run unit tests for both subprojects via ctest" ON)

set(BUILD_ROOT   "${CMAKE_SOURCE_DIR}/build")
set(PIPELINE_SRC "${CMAKE_SOURCE_DIR}/pipeline_cpp")
set(PIPELINE_BIN "${BUILD_ROOT}/pipeline")
set(PROXY_SRC    "${CMAKE_SOURCE_DIR}/proxy_dll")
set(PROXY_BIN    "${BUILD_ROOT}/proxy")
set(STAGE_DIR    "${BUILD_ROOT}/install")

set(PIPELINE_TC "${PIPELINE_BIN}/generators/conan_toolchain.cmake")
set(PROXY_TC    "${PROXY_BIN}/generators/conan_toolchain.cmake")
if(NOT EXISTS "${PIPELINE_TC}" OR NOT EXISTS "${PROXY_TC}")
    message(FATAL_ERROR
        "Conan toolchains not found in ${BUILD_ROOT}.\n"
        "Run `python build.py` once to install x64 and x86 dependencies.")
endif()

add_custom_target(pipeline ALL
    COMMAND "${CMAKE_COMMAND}" --preset conan-windows-x64-release
    COMMAND "${CMAKE_COMMAND}" --build --preset conan-windows-x64-release
    WORKING_DIRECTORY "${PIPELINE_SRC}"
    COMMENT "Building pipeline_cpp (x64 Release)"
    USES_TERMINAL
)

add_custom_target(proxy ALL
    COMMAND "${CMAKE_COMMAND}" --preset conan-windows-x86-release
    COMMAND "${CMAKE_COMMAND}" --build --preset conan-windows-x86-release
    WORKING_DIRECTORY "${PROXY_SRC}"
    COMMENT "Building proxy_dll (Win32 Release)"
    USES_TERMINAL
)

if(ENABLE_TESTS)
    enable_testing()
    add_test(
        NAME pipeline_cpp
        COMMAND "${CMAKE_CTEST_COMMAND}" --preset conan-windows-x64-release
        WORKING_DIRECTORY "${PIPELINE_SRC}"
    )
    add_test(
        NAME proxy_dll
        COMMAND "${CMAKE_CTEST_COMMAND}" --preset conan-windows-x86-release
        WORKING_DIRECTORY "${PROXY_SRC}"
    )
endif()

install(CODE "
    message(STATUS \"Staging pipeline_cpp -> ${STAGE_DIR}\")
    execute_process(
        COMMAND \"${CMAKE_COMMAND}\" --install \"${PIPELINE_BIN}\" --config Release --prefix \"${STAGE_DIR}\"
        RESULT_VARIABLE rc1
    )
    if(rc1)
        message(FATAL_ERROR \"pipeline_cpp install failed: \${rc1}\")
    endif()

    message(STATUS \"Staging proxy_dll -> ${STAGE_DIR}\")
    execute_process(
        COMMAND \"${CMAKE_COMMAND}\" --install \"${PROXY_BIN}\" --config Release --prefix \"${STAGE_DIR}\"
        RESULT_VARIABLE rc2
    )
    if(rc2)
        message(FATAL_ERROR \"proxy_dll install failed: \${rc2}\")
    endif()
")
