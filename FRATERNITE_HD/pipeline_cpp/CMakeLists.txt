cmake_minimum_required(VERSION 3.23)
project(FraternitePipeline LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# /utf-8: JP literals in the glossary; /bigobj: SYSTEM_PROMPT + boost headers.
if(MSVC)
    add_compile_options(/utf-8 /bigobj)
endif()

option(ENABLE_TESTS "Build the GoogleTest suite" ON)

# No Boost.Regex: patterns are Unicode-sensitive, so all are hand-rolled.
find_package(Boost REQUIRED COMPONENTS json program_options)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)

# Tests need an absolute anchor for fixtures; apps must NOT get one
# (a compiled-in path leaks this machine's layout into installed binaries).
get_filename_component(FRAT_PROJECT_DIR_ABS "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# frat_core: shared by apps and tests.
add_library(frat_core STATIC
    src/common/util.cpp
    src/common/util.h
    src/yuris/ypf_archive.cpp
    src/yuris/ypf_archive.h
    src/yuris/cp932_scan.cpp
    src/yuris/cp932_scan.h
    src/extract/extractor.cpp
    src/extract/extractor.h
    src/build_tsv/build_tsv.cpp
    src/build_tsv/build_tsv.h
    src/build_tsv/name_fixups.cpp
    src/build_tsv/name_fixups.h
    src/translate/glossary.cpp
    src/translate/glossary.h
    src/translate/anthropic_client.cpp
    src/translate/anthropic_client.h
    src/translate/speaker_gate.cpp
    src/translate/speaker_gate.h
    src/translate/translate_core.cpp
    src/translate/translate_core.h
)
target_include_directories(frat_core PUBLIC src)
target_compile_definitions(frat_core PUBLIC
    _CRT_SECURE_NO_WARNINGS
    _WIN32_WINNT=0x0601
)
target_link_libraries(frat_core PUBLIC
    Boost::json
    # NOT boost::boost -- it pulls test_exec_monitor whose main() clashes with gtest.
    Boost::headers
    openssl::openssl
    ZLIB::ZLIB
    ws2_32
    crypt32
)

# Pipeline executables.  Program_options scoped to apps, not frat_core.
function(frat_app target main)
    add_executable(${target} ${main} src/apps/paths.h src/apps/paths.cpp)
    target_link_libraries(${target} PRIVATE frat_core Boost::program_options)
endfunction()

frat_app(00_run_all    src/apps/run_all_main.cpp)
frat_app(01_extract    src/apps/extract_main.cpp)
frat_app(02_translate  src/apps/translate_main.cpp)

# Install: just the three apps; all deps are static.
install(TARGETS 00_run_all 01_extract 02_translate
        RUNTIME DESTINATION bin)

# Tests
if(ENABLE_TESTS)
    find_package(GTest REQUIRED)
    enable_testing()
    add_subdirectory(tests)
endif()
