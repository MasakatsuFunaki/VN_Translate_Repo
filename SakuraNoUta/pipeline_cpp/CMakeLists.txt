cmake_minimum_required(VERSION 3.23)
project(SakuraNoUtaPipeline LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC defaults to interpreting source bytes as the system codepage,
# which mangles UTF-8 Japanese literals.  /utf-8 fixes source and execution
# charset; /bigobj lifts the section limit for Boost + glossary translation units.
if(MSVC)
    add_compile_options(/utf-8 /bigobj)
endif()

option(ENABLE_TESTS "Build the GoogleTest suite" ON)

find_package(Boost REQUIRED COMPONENTS json regex program_options)
find_package(OpenSSL REQUIRED)
find_package(freetype REQUIRED)
find_package(stb REQUIRED)

# Component and system tests need an absolute anchor for local corpus and tool
# paths.  Installed executables must NOT receive a compiled-in path; they locate
# the project folder dynamically via src/apps/paths.cpp.
get_filename_component(SNU_PROJECT_DIR_ABS "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# Shared core library compiled from pipeline modules plus the proxy DLL's own
# text layout logic so TSV table validation measures lines identically to the
# runtime hook.
get_filename_component(SNU_RUNTIME_TEXT_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../proxy_dll/src" ABSOLUTE)

add_library(snu_core STATIC
    ${SNU_RUNTIME_TEXT_DIR}/translator_logic.cpp
    ${SNU_RUNTIME_TEXT_DIR}/translator_logic.h
    src/common/util.cpp
    src/common/util.h
    src/common/coverage.cpp
    src/common/coverage.h
    src/bgi/bgi_archive.cpp
    src/bgi/bgi_archive.h
    src/bgi/bgi_cbg.cpp
    src/bgi/bgi_cbg.h
    src/extract/extractor.cpp
    src/extract/extractor.h
    src/build_tsv/build_tsv.cpp
    src/build_tsv/build_tsv.h
    src/build_tsv/box_budget.cpp
    src/build_tsv/box_budget.h
    src/cache/glossary.cpp
    src/cache/glossary.h
    src/cache/speaker_gate.cpp
    src/cache/speaker_gate.h
    src/cache/cache_core.cpp
    src/cache/cache_core.h
    src/cg/image.cpp
    src/cg/image.h
    src/cg/text_render.cpp
    src/cg/text_render.h
    src/cg/cg_pipeline.cpp
    src/cg/cg_pipeline.h
)
target_include_directories(snu_core PUBLIC src ${SNU_RUNTIME_TEXT_DIR})
target_compile_definitions(snu_core PUBLIC
    _CRT_SECURE_NO_WARNINGS
    _WIN32_WINNT=0x0601
)
target_link_libraries(snu_core PUBLIC
    Boost::json
    Boost::regex
    Boost::headers
    openssl::openssl
    Freetype::Freetype
    stb::stb
    ws2_32
    crypt32
)

function(snu_app target main)
    add_executable(${target} ${main} src/apps/paths.h src/apps/paths.cpp)
    target_link_libraries(${target} PRIVATE snu_core Boost::program_options shell32)
endfunction()

snu_app(00_run_all              src/apps/run_all_main.cpp)
snu_app(01_extract              src/apps/extract_main.cpp)
snu_app(02_build_tsv            src/apps/build_tsv_main.cpp)
snu_app(03_find_narrative_cg    src/apps/find_narrative_cg_main.cpp)

install(TARGETS 00_run_all 01_extract 02_build_tsv 03_find_narrative_cg
        RUNTIME DESTINATION bin)

if(ENABLE_TESTS)
    find_package(GTest REQUIRED)
    enable_testing()
    add_subdirectory(tests)
endif()
