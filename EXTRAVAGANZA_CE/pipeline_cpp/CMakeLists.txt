cmake_minimum_required(VERSION 3.23)
project(ExtravaganzaCePipeline LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC defaults to interpreting source bytes as the system codepage, which
# mangles the UTF-8 Japanese literals in the glossary.  /utf-8 is the standard
# fix.  /bigobj: glossary + boost headers overflow the default section limit.
if(MSVC)
    add_compile_options(/utf-8 /bigobj)
endif()

option(ENABLE_TESTS "Build the GoogleTest suite" ON)

find_package(Boost REQUIRED COMPONENTS json regex program_options)
find_package(OpenSSL REQUIRED)

# No project folder is compiled in: a build-time absolute path would ship this
# machine's directory layout inside every binary.  The apps walk up from their
# own executable to find it instead (src/apps/paths.cpp), and --dir overrides.

# --------------------------------------------------------------------
# exc_core: everything except the app mains, shared by apps and tests.
# --------------------------------------------------------------------
add_library(exc_core STATIC
    src/common/util.cpp
    src/common/util.h
    src/spt/spt_script.cpp
    src/spt/spt_script.h
    src/extract/extractor.cpp
    src/extract/extractor.h
    src/repack/repack.cpp
    src/repack/repack.h
    src/charts/charts.cpp
    src/charts/charts.h
    src/translate/glossary.cpp
    src/translate/glossary.h
    src/translate/anthropic_client.cpp
    src/translate/anthropic_client.h
    src/translate/translate_core.cpp
    src/translate/translate_core.h
    src/translate/speaker_gate.cpp
    src/translate/speaker_gate.h
    src/xtx/xtx.cpp
    src/xtx/xtx.h
    src/xtx/xtx_tables.cpp
)
target_include_directories(exc_core PUBLIC src)
target_compile_definitions(exc_core PUBLIC
    _CRT_SECURE_NO_WARNINGS
    _WIN32_WINNT=0x0601
)
target_link_libraries(exc_core PUBLIC
    Boost::json
    Boost::regex
    # Beast/Asio only.  NOT the boost::boost aggregate -- that pulls in every
    # compiled Boost lib, including test_exec_monitor, whose main() wants a
    # test_main symbol the GoogleTest suites don't define.
    Boost::headers
    openssl::openssl
    ws2_32
    crypt32
)

# --------------------------------------------------------------------
# Pipeline executables (numbered in run order).
#
# The spine is 01_extract -> 02_translate -> deploy; the speaker gate is part
# of the translation step rather than an app of its own, so it cannot be
# forgotten before a paid run.  03..05 write the English into the game's own
# files and 00_run_all runs them between translation and the deploy copy.
# --------------------------------------------------------------------
# Only the app mains parse a command line (src/apps/paths.h), so
# Program_options is scoped here rather than pushed through exc_core.
function(exc_app target main)
    add_executable(${target} ${main} src/apps/paths.h src/apps/paths.cpp)
    target_link_libraries(${target} PRIVATE exc_core Boost::program_options)
endfunction()

exc_app(00_run_all              src/apps/run_all_main.cpp)
exc_app(01_extract              src/apps/extract_main.cpp)
exc_app(02_translate            src/apps/translate_main.cpp)
exc_app(03_repack               src/apps/repack_main.cpp)
exc_app(04_translate_xtx        src/apps/translate_xtx_main.cpp)
exc_app(05_translate_charts     src/apps/translate_charts_main.cpp)

# --------------------------------------------------------------------
# Install: what the USER runs.  bin/ only -- exc_core and the test binaries
# are internal, and the proxy half installs its DLL into game/ so a 32-bit
# winmm.dll never lands beside these x64 executables.
# --------------------------------------------------------------------
install(TARGETS 00_run_all 01_extract 02_translate 03_repack 04_translate_xtx
                05_translate_charts
        RUNTIME DESTINATION bin)

# --------------------------------------------------------------------
# Tests
# --------------------------------------------------------------------
if(ENABLE_TESTS)
    find_package(GTest REQUIRED)
    enable_testing()
    add_subdirectory(tests)
endif()
