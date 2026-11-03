cmake_minimum_required(VERSION 3.23)
project(KishinHishoTools LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# /utf-8: the recon report contains an em dash and 03's report contains arrows,
# and both are diffed byte-for-byte against a committed log.
# /W4: item 1 of the house C++ standard -- compile cleanly at high warning levels.
if(MSVC)
    add_compile_options(/utf-8 /W4)
endif()

option(ENABLE_TESTS "Build the GoogleTest suite" ON)

find_package(Boost REQUIRED COMPONENTS json regex program_options)
find_package(capstone REQUIRED)
find_package(ZLIB REQUIRED)

# The tests -- and only the tests -- need an absolute anchor for the committed
# fixtures and golden logs.  The apps must NOT get one: a compiled-in path
# ships this machine's directory layout inside every installed binary, so they
# find the project by walking up from their own executable (src/apps/paths.cpp).
get_filename_component(KSH_PROJECT_DIR_ABS "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# --------------------------------------------------------------------
# ksh_core: everything except the app mains, shared by apps and tests.
# --------------------------------------------------------------------
add_library(ksh_core STATIC
    src/common/util.cpp
    src/common/util.h
    src/pe/pe_image.cpp
    src/pe/pe_image.h
    src/pe/disasm.cpp
    src/pe/disasm.h
    src/minidump/minidump.cpp
    src/minidump/minidump.h
    src/patch/deploy.cpp
    src/patch/deploy.h
    src/pak/pak_archive.cpp
    src/pak/pak_archive.h
    src/pak/nps_rebuilder.cpp
    src/pak/nps_rebuilder.h
    src/analysis/recon.cpp
    src/analysis/dumpreport.cpp
    src/analysis/reports.h
)
target_include_directories(ksh_core PUBLIC src)
target_compile_definitions(ksh_core PUBLIC
    _CRT_SECURE_NO_WARNINGS
    _WIN32_WINNT=0x0601
)
target_link_libraries(ksh_core PUBLIC
    Boost::json
    Boost::regex
    capstone::capstone
    ZLIB::ZLIB
)

# --------------------------------------------------------------------
# Tools: the deploy driver plus one executable per analysis stage.
# --------------------------------------------------------------------
function(ksh_app target main)
    add_executable(${target} ${main} src/apps/paths.h src/apps/paths.cpp)
    target_link_libraries(${target} PRIVATE ksh_core Boost::program_options)
endfunction()

ksh_app(01_patch                     src/apps/patch_main.cpp)
ksh_app(02_build_pak                 src/apps/build_pak_main.cpp)
ksh_app(analysis_01_recon            src/apps/recon_main.cpp)
ksh_app(analysis_02_decrypt_devnames src/apps/decrypt_devnames_main.cpp)
ksh_app(analysis_03_parse_dump       src/apps/parse_dump_main.cpp)
ksh_app(analysis_04_extract_thunk    src/apps/extract_thunk_main.cpp)
ksh_app(analysis_05_trace_stage3     src/apps/trace_stage3_main.cpp)
ksh_app(analysis_06_parse_recognizedisc src/apps/parse_recognizedisc_main.cpp)

# --------------------------------------------------------------------
# Install: bin/ is what the user runs.
#
# The root project collects both halves into build/install, and the two
# halves must not share a folder -- helper_dll is 32-bit and carries a DLL
# the loader would happily pick up next to these 64-bit executables. So the
# Win32 artifacts install into game/ and these into bin/.
#
# ksh_core is an internal static library and the ut_* binaries are tests:
# neither is user-facing, so neither is installed.
# --------------------------------------------------------------------
install(TARGETS
            01_patch
            02_build_pak
            analysis_01_recon
            analysis_02_decrypt_devnames
            analysis_03_parse_dump
            analysis_04_extract_thunk
            analysis_05_trace_stage3
            analysis_06_parse_recognizedisc
        RUNTIME DESTINATION bin)

# --------------------------------------------------------------------
# Tests
# --------------------------------------------------------------------
if(ENABLE_TESTS)
    find_package(GTest REQUIRED)
    enable_testing()
    add_subdirectory(tests)
endif()
