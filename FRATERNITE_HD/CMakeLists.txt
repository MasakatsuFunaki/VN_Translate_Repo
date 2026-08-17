# FRATERNITE_HD -- root build.
# Two halves (pipeline_cpp x64, proxy_dll Win32) need separate toolchains,
# so this project drives each as an ExternalProject.  Use build.py.

cmake_minimum_required(VERSION 3.23)
project(FraterniteHD LANGUAGES NONE)

include(ExternalProject)

set(FRAT_CONFIG Release CACHE STRING "Configuration both halves are built in")

# Fail at configure time, not mid-build, if Conan is missing.
find_program(FRAT_CONAN NAMES conan REQUIRED)

# frat_half(<name> SOURCE <dir> PRESET <preset> PROFILE <conan profile>)
# Runs Conan then configures through the half's own preset, so the human
# preset and the automated one cannot drift.
function(frat_half name)
    cmake_parse_arguments(H "" "SOURCE;PRESET;PROFILE" "" ${ARGN})

    set(src ${CMAKE_CURRENT_SOURCE_DIR}/${H_SOURCE})
    set(bin ${CMAKE_BINARY_DIR}/${name})

    # Ninja picks arch from the compiler env, not from CMake, so each half
    # must build inside the vcvars environment its Conan profile selects.
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
cmake --build \"${bin_win}\" --config ${FRAT_CONFIG}\r
")

    ExternalProject_Add(${name}
        SOURCE_DIR      ${src}
        BINARY_DIR      ${bin}
        # Keep EP bookkeeping inside the half's own folder.
        PREFIX          ${bin}
        TMP_DIR         ${bin}/_ep
        STAMP_DIR       ${bin}/_ep
        INSTALL_DIR     ${CMAKE_BINARY_DIR}/install
        DOWNLOAD_COMMAND ""
        CONFIGURE_COMMAND
            ${FRAT_CONAN} install ${src} --output-folder=${bin}
                --build=missing -pr=${H_PROFILE} -s build_type=${FRAT_CONFIG}
                -c "tools.cmake.cmaketoolchain:generator=Ninja Multi-Config"
        COMMAND
            ${configure_bat}
        BUILD_COMMAND
            ${build_bat}
        # Let the inner build decide what is out of date.
        BUILD_ALWAYS    ON
        # Aggregate install below owns this step.
        INSTALL_COMMAND ""
        USES_TERMINAL_CONFIGURE ON
        USES_TERMINAL_BUILD     ON
    )
endfunction()

frat_half(pipeline
    SOURCE  pipeline_cpp
    PRESET  conan-windows-x64-release
    PROFILE default)

# Absolute path: a relative one resolves against the EP binary dir.
frat_half(proxy
    SOURCE  proxy_dll
    PRESET  conan-windows-x86-release
    PROFILE ${CMAKE_CURRENT_SOURCE_DIR}/proxy_dll/profiles/x86)

# Aggregate install: both halves land in one prefix (bin/ + game/).
foreach(half IN ITEMS pipeline proxy)
    install(CODE "
        message(STATUS \"Installing ${half} -> \${CMAKE_INSTALL_PREFIX}\")
        execute_process(
            COMMAND \"${CMAKE_COMMAND}\" --install \"${CMAKE_BINARY_DIR}/${half}\"
                    --config ${FRAT_CONFIG} --prefix \"\${CMAKE_INSTALL_PREFIX}\"
            COMMAND_ERROR_IS_FATAL ANY)
    ")
endforeach()
