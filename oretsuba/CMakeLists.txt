cmake_minimum_required(VERSION 3.23)
project(Oretsuba LANGUAGES NONE)

include(ExternalProject)

set(ORETSUBA_CONFIG Release CACHE STRING "Configuration both halves are built in")

find_program(ORETSUBA_CONAN NAMES conan REQUIRED)

function(oretsuba_half name)
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
cmake --build \"${bin_win}\" --config ${ORETSUBA_CONFIG}\r
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
            ${ORETSUBA_CONAN} install ${src} --output-folder=${bin}
                --build=missing -pr=${H_PROFILE} -s build_type=${ORETSUBA_CONFIG}
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

set(ORETSUBA_HALVES pipeline)

oretsuba_half(pipeline
    SOURCE  pipeline_cpp
    PRESET  conan-windows-x64-release
    PROFILE default)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/proxy_dll/CMakeLists.txt")
    oretsuba_half(proxy
        SOURCE  proxy_dll
        PRESET  conan-windows-x86-release
        PROFILE ${CMAKE_CURRENT_SOURCE_DIR}/proxy_dll/profiles/x86)
    list(APPEND ORETSUBA_HALVES proxy)
endif()

foreach(half IN LISTS ORETSUBA_HALVES)
    install(CODE "
        message(STATUS \"Installing ${half} -> \${CMAKE_INSTALL_PREFIX}\")
        execute_process(
            COMMAND \"${CMAKE_COMMAND}\" --install \"${CMAKE_BINARY_DIR}/${half}\"
                    --config ${ORETSUBA_CONFIG} --prefix \"\${CMAKE_INSTALL_PREFIX}\"
            COMMAND_ERROR_IS_FATAL ANY)
    ")
endforeach()
