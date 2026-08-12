# EXTRAVAGANZA_CE -- root build for the whole game.
#
# The game is built from two halves that CANNOT share one CMake configure:
# `pipeline_cpp` is x64 (the tools that read the SPT archives, call the API and
# repack), `proxy_dll` is Win32 because it is injected into mushiEx_CE.exe, a
# 32-bit process.  A CMake project has exactly one toolchain, so this project
# compiles nothing itself.  It drives each half as an ExternalProject with its
# own Conan profile, its own toolchain file and its own binary directory under
# build/, and it owns the aggregate install that collects both halves into
# build/install.
#
#   cmake --preset windows-release
#   cmake --build   --preset windows-release
#   cmake --install build --config Release
#
# `build.py` is the supported front end and wraps exactly those three
# commands.

cmake_minimum_required(VERSION 3.23)
project(ExtravaganzaCE LANGUAGES NONE)

include(ExternalProject)

set(EXC_CONFIG Release CACHE STRING "Configuration both halves are built in")

# Conan runs inside each half's configure step.  Resolving it now turns a
# missing Conan into a configure error instead of a failure halfway through
# a build.
find_program(EXC_CONAN NAMES conan REQUIRED)

# --------------------------------------------------------------------
# exc_half(<name> SOURCE <dir> PRESET <preset> PROFILE <conan profile>)
#
# Adds one architecture half.  Its binary directory is build/<name>, so the
# whole game has a single build folder with one subtree per architecture.
#
# The configure step runs Conan first -- it writes the toolchain the preset
# points at -- and then configures through the half's own CMakePresets.json.
# Going through the preset is deliberate: the preset a human types and the
# configure this project performs are then the same one, and cannot drift.
# The wrapper's `cd /d` into the half's source directory is needed because
# `cmake --preset` reads the preset files from the current working directory.
# --------------------------------------------------------------------
function(exc_half name)
    cmake_parse_arguments(H "" "SOURCE;PRESET;PROFILE" "" ${ARGN})

    set(src ${CMAKE_CURRENT_SOURCE_DIR}/${H_SOURCE})
    set(bin ${CMAKE_BINARY_DIR}/${name})

    # Ninja takes its target architecture from the compiler environment rather
    # than from CMake -- there is no equivalent of the Visual Studio
    # generator's `architecture` field -- so each half must configure and build
    # inside the vcvars environment its own Conan profile selects: x64 for the
    # pipeline, x86 for the proxy.  Conan writes that environment as
    # conanvcvars.bat beside the toolchain during the install step below.
    # It has to be applied through a wrapper script because a batch file cannot
    # export its environment back into the calling process.
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
cmake --build \"${bin_win}\" --config ${EXC_CONFIG}\r
")

    ExternalProject_Add(${name}
        SOURCE_DIR      ${src}
        BINARY_DIR      ${bin}
        # Keep ExternalProject's bookkeeping inside the half's own folder;
        # the defaults would scatter <name>-prefix/ directories through
        # build/.
        PREFIX          ${bin}
        TMP_DIR         ${bin}/_ep
        STAMP_DIR       ${bin}/_ep
        INSTALL_DIR     ${CMAKE_BINARY_DIR}/install
        DOWNLOAD_COMMAND ""
        CONFIGURE_COMMAND
            ${EXC_CONAN} install ${src} --output-folder=${bin}
                --build=missing -pr=${H_PROFILE} -s build_type=${EXC_CONFIG}
                -c "tools.cmake.cmaketoolchain:generator=Ninja Multi-Config"
        COMMAND
            ${configure_bat}
        BUILD_COMMAND
            ${build_bat}
        # Sources change far more often than the configure does; let the
        # inner build decide what is out of date.
        BUILD_ALWAYS    ON
        # The aggregate install below owns this half's install step.
        INSTALL_COMMAND ""
        USES_TERMINAL_CONFIGURE ON
        USES_TERMINAL_BUILD     ON
    )
endfunction()

exc_half(pipeline
    SOURCE  pipeline_cpp
    PRESET  conan-windows-x64-release
    PROFILE default)

# The profile path must be ABSOLUTE: a relative one resolves against the
# working directory, and ExternalProject runs its steps from the binary dir.
exc_half(proxy
    SOURCE  proxy_dll
    PRESET  conan-windows-x86-release
    PROFILE ${CMAKE_CURRENT_SOURCE_DIR}/proxy_dll/profiles/x86)

# --------------------------------------------------------------------
# Aggregate install.
#
# `cmake --install build --config Release` runs each half's own install
# script into this project's prefix, so the artifacts of both architectures
# land in one tree.  Each half decides what is user-facing; this file only
# decides where the halves meet.
# --------------------------------------------------------------------
foreach(half IN ITEMS pipeline proxy)
    install(CODE "
        message(STATUS \"Installing ${half} -> \${CMAKE_INSTALL_PREFIX}\")
        execute_process(
            COMMAND \"${CMAKE_COMMAND}\" --install \"${CMAKE_BINARY_DIR}/${half}\"
                    --config ${EXC_CONFIG} --prefix \"\${CMAKE_INSTALL_PREFIX}\"
            COMMAND_ERROR_IS_FATAL ANY)
    ")
endforeach()
