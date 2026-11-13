cmake_minimum_required(VERSION 3.23)
project(OretsubaPipeline LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /utf-8 /bigobj /external:anglebrackets /external:W0)
endif()

option(ENABLE_TESTS "Build the GoogleTest suite" ON)

find_package(Boost REQUIRED COMPONENTS json regex program_options)
find_package(OpenSSL REQUIRED)

get_filename_component(ORETSUBA_PROJECT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

add_library(oretsuba_core STATIC
    src/apps/paths.cpp
    src/apps/paths.h
    src/common/util.cpp
    src/common/util.h
    src/lpk/lpk_archive.cpp
    src/lpk/lpk_archive.h
    src/sob/sob_parser.cpp
    src/sob/sob_parser.h
    src/extract/extractor.cpp
    src/extract/extractor.h
    src/translate/glossary.cpp
    src/translate/glossary.h
    src/translate/gemini_client.cpp
    src/translate/gemini_client.h
    src/translate/speaker_gate.cpp
    src/translate/speaker_gate.h
    src/translate/translate_core.cpp
    src/translate/translate_core.h
    src/build_tsv/build_tsv.cpp
    src/build_tsv/build_tsv.h
)
target_include_directories(oretsuba_core PUBLIC src)
target_compile_definitions(oretsuba_core PUBLIC
    _CRT_SECURE_NO_WARNINGS
    _WIN32_WINNT=0x0601
)
target_link_libraries(oretsuba_core PUBLIC
    Boost::json
    Boost::regex
    Boost::program_options
    Boost::headers
    openssl::openssl
    ws2_32
    crypt32
    shell32
)

function(oretsuba_app target main)
    add_executable(${target} ${main})
    target_link_libraries(${target} PRIVATE oretsuba_core)
endfunction()

oretsuba_app(00_run_all src/apps/run_all_main.cpp)
oretsuba_app(01_extract src/apps/extract_main.cpp)
oretsuba_app(02_translate src/apps/translate_main.cpp)
oretsuba_app(03_build_tsv src/apps/build_tsv_main.cpp)

install(TARGETS 00_run_all 01_extract 02_translate 03_build_tsv RUNTIME DESTINATION bin)

if(ENABLE_TESTS)
    find_package(GTest REQUIRED)
    enable_testing()
    add_subdirectory(tests)
endif()
