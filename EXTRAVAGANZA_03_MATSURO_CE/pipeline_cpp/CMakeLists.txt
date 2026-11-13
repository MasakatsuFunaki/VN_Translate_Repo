cmake_minimum_required(VERSION 3.23)
project(ExtravaganzaMatsuroPipeline LANGUAGES CXX)

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

# exm_core: everything except the app mains, shared by apps and tests.
add_library(exm_core STATIC
    src/common/util.cpp
    src/common/util.h
    src/spt/spt_script.cpp
    src/spt/spt_script.h
    src/extract/extractor.cpp
    src/extract/extractor.h
    src/build_tsv/build_tsv.cpp
    src/build_tsv/build_tsv.h
    src/cache/glossary.cpp
    src/cache/glossary.h
    src/cache/speaker_gate.cpp
    src/cache/speaker_gate.h
    src/cache/cache_core.cpp
    src/cache/cache_core.h
    src/xtx/xtx.cpp
    src/xtx/xtx.h
    src/xtx/xtx_tables.cpp
    src/menu/menu_patcher.cpp
    src/menu/menu_patcher.h
)
target_include_directories(exm_core PUBLIC src)
target_compile_definitions(exm_core PUBLIC
    _CRT_SECURE_NO_WARNINGS
    _WIN32_WINNT=0x0601
)
target_link_libraries(exm_core PUBLIC
    Boost::json
    Boost::regex
    # Beast/Asio only.  NOT the boost::boost aggregate -- that pulls in every
    # compiled Boost lib, including test_exec_monitor, whose main() wants a
    # test_main symbol the GoogleTest suites don't define.
    Boost::headers
    openssl::openssl
    ws2_32
    crypt32
    windowscodecs
    ole32
)

# Pipeline executables, numbered by the order they run in.
# Only the app mains parse a command line (src/apps/paths.h), so
# Program_options and shell32 are scoped here rather than pushed through exm_core.
function(exm_app target main)
    add_executable(${target} ${main} src/apps/paths.h src/apps/paths.cpp)
    target_link_libraries(${target} PRIVATE exm_core Boost::program_options shell32)
endfunction()

exm_app(00_run_all              src/apps/run_all_main.cpp)
exm_app(01_extract              src/apps/extract_main.cpp)
exm_app(02_build_tsv            src/apps/build_tsv_main.cpp)
exm_app(03_build_xtx            src/apps/build_xtx_main.cpp)
exm_app(04_patch_menus          src/apps/patch_menus_main.cpp)
exm_app(cp932_probe             src/apps/cp932_probe_main.cpp)

install(TARGETS 00_run_all 01_extract 02_build_tsv 03_build_xtx 04_patch_menus
        RUNTIME DESTINATION bin)

if(ENABLE_TESTS)
    find_package(GTest REQUIRED)
    enable_testing()
    add_subdirectory(tests)
endif()
