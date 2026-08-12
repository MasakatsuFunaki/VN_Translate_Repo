cmake_minimum_required(VERSION 3.23)
project(ShingakkouPipeline LANGUAGES CXX)

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
# Narrative-CG step only: FreeType for glyph measurement + rasterisation, stb
# for the BMP/PNG codecs.
find_package(freetype REQUIRED)
find_package(stb REQUIRED)

# The tests -- and only the tests -- need an absolute anchor for the real
# game-data fixtures.  The apps must NOT get one: a compiled-in path ships
# this machine's directory layout inside every installed binary, so they find
# the project by walking up from their own executable (src/apps/paths.cpp).
get_filename_component(SHIN_PROJECT_DIR_ABS "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# --------------------------------------------------------------------
# shin_core: everything except the app mains, shared by apps and tests.
# --------------------------------------------------------------------
add_library(shin_core STATIC
    src/common/util.cpp
    src/common/util.h
    src/ddp/ddp_archive.cpp
    src/ddp/ddp_archive.h
    src/extract/extractor.cpp
    src/extract/extractor.h
    src/build_tsv/build_tsv.cpp
    src/build_tsv/build_tsv.h
    src/translate/glossary.cpp
    src/translate/glossary.h
    src/translate/anthropic_client.cpp
    src/translate/anthropic_client.h
    src/translate/speaker_gate.cpp
    src/translate/speaker_gate.h
    src/translate/translate_core.cpp
    src/translate/translate_core.h
    src/cg/image.cpp
    src/cg/image.h
    src/cg/text_render.cpp
    src/cg/text_render.h
    src/cg/cg_pipeline.cpp
    src/cg/cg_pipeline.h
)
target_include_directories(shin_core PUBLIC src)
target_compile_definitions(shin_core PUBLIC
    _CRT_SECURE_NO_WARNINGS
    _WIN32_WINNT=0x0601
)
target_link_libraries(shin_core PUBLIC
    Boost::json
    Boost::regex
    # Beast/Asio only.  NOT the boost::boost aggregate -- that pulls in every
    # compiled Boost lib, including test_exec_monitor, whose main() wants a
    # test_main symbol the GoogleTest suites don't define.
    Boost::headers
    openssl::openssl
    Freetype::Freetype
    stb::stb
    ws2_32
    crypt32
)

# --------------------------------------------------------------------
# Pipeline executables, numbered by the order they run in.
#
# 00_run_all performs 01 and 02 and then deploys; the numbered pair is the
# manual path.  02 carries the speaker gate and the runtime-table build with
# it, so there is no separate step to remember between translating and
# playing.  03 is a side tool: it repaints text baked into the CG archives and
# has nothing to do with the script, which is why 00_run_all never runs it.
# --------------------------------------------------------------------
# Only the app mains parse a command line (src/apps/paths.h), so
# Program_options is scoped here rather than pushed through shin_core.
function(shin_app target main)
    add_executable(${target} ${main} src/apps/paths.h src/apps/paths.cpp)
    target_link_libraries(${target} PRIVATE shin_core Boost::program_options)
endfunction()

shin_app(00_run_all             src/apps/run_all_main.cpp)
shin_app(01_extract             src/apps/extract_main.cpp)
shin_app(02_translate           src/apps/translate_main.cpp)
shin_app(03_find_narrative_cg   src/apps/find_narrative_cg_main.cpp)

# Verification-only: dumps the DDP entry table and drives the repacker so a
# rebuilt archive can be byte-compared.  Not part of the pipeline, and not
# installed -- it is driven by the offline harness in analysys/.
add_executable(cg_parity src/apps/cg_parity_main.cpp)
target_link_libraries(cg_parity PRIVATE shin_core)

# --------------------------------------------------------------------
# Install: the four executables a user runs, and nothing else.  shin_core,
# cg_parity and the test binaries exist to produce and check these, and stay
# in the build tree.  Every dependency is a static library, so there is no
# runtime to carry.
# --------------------------------------------------------------------
install(TARGETS 00_run_all 01_extract 02_translate 03_find_narrative_cg
        RUNTIME DESTINATION bin)

# --------------------------------------------------------------------
# Tests
# --------------------------------------------------------------------
if(ENABLE_TESTS)
    find_package(GTest REQUIRED)
    enable_testing()
    add_subdirectory(tests)
endif()
