# Ren_mushi_CE -- root build for the whole game.
#
# The game is built from two halves that cannot share one CMake configure:
# `pipeline_cpp` is x64 (the tools that read the SPT archives and call the
# API), `proxy_dll` is Win32 because it is injected into a 32-bit BLACKCyc
# game process. A CMake project has exactly one toolchain, so this project
# compiles nothing itself. It drives each half as an ExternalProject with its
# own Conan profile, its own toolchain file and its own binary directory under
# build/, and it owns the aggregate install that collects both halves into
# build/install.
#
#   cmake --preset windows-release
#   cmake --build   --preset windows-release
#   cmake --install build --config Release
#
# `build.py` is the supported front end and wraps exactly those three
# commands. See SakuraNoUta/BUILD_STRUCTURE.md for the layout and its
# conventions.

cmake_minimum_required(VERSION 3.23)
project(RenMushiCE LANGUAGES NONE)

include(ExternalProject)

set(REN_CONFIG Release CACHE STRING "Configuration both halves are built in")

find_program(REN_CONAN NAMES conan REQUIRED)

function(ren_half name)
    cmake_parse_arguments(H "" "SOURCE;PRESET;PROFILE" "" ${ARGN})

    set(src ${CMAKE_CURRENT_SOURCE_DIR}/${H_SOURCE})
    set(bin ${CMAKE_BINARY_DIR}/${name})

    file(TO_NATIVE_PATH ${bin}/generators/conanvcvars.bat vcvars_bat)
    file(TO_NATIVE_PATH ${src} src_win)
    file(TO_NATIVE_PATH ${bin} bin_win)

    set(wrap ${CMAKE_BINARY_DIR}/_wrappers)
    set(configure_bat ${wrap}/${name}-configure.bat)
    set(build_bat     ${wrap}/${name}-build.bat)

    file(WRITE ${configure_bat}
"@echo off\r
call \"${vcvars_bat}\" || exit /b 1\r
cd /d \"${src_win}\" || exit /b 1\r
cmake --preset ${H_PRESET}\r
")
    file(WRITE ${build_bat}
"@echo off\r
call \"${vcvars_bat}\" || exit /b 1\r
cmake --build \"${bin_win}\" --config ${REN_CONFIG}\r
")

    ExternalProject_Add(${name}
        SOURCE_DIR      ${src}
        BINARY_DIR      ${bin}
        PREFIX          ${bin}
        TMP_DIR         ${bin}/_ep
        STAMP_DIR       ${bin}/_ep
        INSTALL_DIR     ${CMAKE_BINARY_DIR}/install
        DOWNLOAD_COMMAND ""
        CONFIGURE_COMMAND
            ${REN_CONAN} install ${src} --output-folder=${bin}
                --build=missing -pr=${H_PROFILE} -s build_type=${REN_CONFIG}
                -c "tools.cmake.cmaketoolchain:generator=Ninja Multi-Config"
        COMMAND
            ${configure_bat}
        BUILD_COMMAND
            ${build_bat}
        BUILD_ALWAYS    ON
        INSTALL_COMMAND ""
        USES_TERMINAL_CONFIGURE ON
        USES_TERMINAL_BUILD     ON
    )
endfunction()

ren_half(pipeline
    SOURCE  pipeline_cpp
    PRESET  conan-windows-x64-release
    PROFILE default)

ren_half(proxy
    SOURCE  proxy_dll
    PRESET  conan-windows-x86-release
    PROFILE ${CMAKE_CURRENT_SOURCE_DIR}/proxy_dll/profiles/x86)

foreach(half IN ITEMS pipeline proxy)
    install(CODE "
        message(STATUS \"Installing ${half} -> \${CMAKE_INSTALL_PREFIX}\")
        execute_process(
            COMMAND \"${CMAKE_COMMAND}\" --install \"${CMAKE_BINARY_DIR}/${half}\"
                    --config ${REN_CONFIG} --prefix \"\${CMAKE_INSTALL_PREFIX}\"
            COMMAND_ERROR_IS_FATAL ANY)
    ")
endforeach()
