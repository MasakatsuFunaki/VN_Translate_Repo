cmake_minimum_required(VERSION 3.23)
project(SakuraNoTokiPipeline LANGUAGES CXX)

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
find_package(stb REQUIRED)

# The tests -- and only the tests -- need an absolute anchor for the real
# game-data fixtures.  The apps must NOT get one: a compiled-in path ships
# this machine's directory layout inside every installed binary, so they find
# the project by walking up from their own executable (src/apps/paths.cpp).
get_filename_component(SNT_PROJECT_DIR_ABS "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# --------------------------------------------------------------------
# snt_core: everything except the app mains, shared by apps and tests.
# --------------------------------------------------------------------
add_library(snt_core STATIC
    src/common/util.cpp
    src/common/util.h
    src/common/coverage.cpp
    src/common/coverage.h
    src/artemis/pfs_archive.cpp
    src/artemis/pfs_archive.h
    src/extract/extractor.cpp
    src/extract/extractor.h
    src/build_tsv/build_tsv.cpp
    src/build_tsv/build_tsv.h
    src/translate/glossary.cpp
    src/translate/glossary.h
    src/translate/anthropic_client.cpp
    src/translate/anthropic_client.h
    src/translate/translate_core.cpp
    src/translate/translate_core.h
    src/translate/speaker_gate.cpp
    src/translate/speaker_gate.h
    src/cg/image.cpp
    src/cg/image.h
    src/cg/cg_pipeline.cpp
    src/cg/cg_pipeline.h
)
target_include_directories(snt_core PUBLIC src)
target_compile_definitions(snt_core PUBLIC
    _CRT_SECURE_NO_WARNINGS
    _WIN32_WINNT=0x0601
)
target_link_libraries(snt_core PUBLIC
    Boost::json
    Boost::regex
    # Beast/Asio only.  NOT the boost::boost aggregate -- that pulls in every
    # compiled Boost lib, including test_exec_monitor, whose main() wants a
    # test_main symbol the GoogleTest suites don't define.
    Boost::headers
    # OpenSSL supplies both TLS for api.anthropic.com and the SHA-1 the PF8
    # archive key is derived from.
    openssl::openssl
    stb::stb
    ws2_32
    crypt32
)

# --------------------------------------------------------------------
# Pipeline executables.
#
# The numbering is the order they run in: extract, then translate, then the
# deploy 00_run_all performs itself.  The speaker gate and the runtime-table
# builder are library code inside the translation step rather than commands of
# their own, so the numbers stop at the narrative-CG scanner, which is a
# separate investigation and not part of the three-step run.
# --------------------------------------------------------------------
# Only the app mains parse a command line (src/apps/paths.h), so
# Program_options is scoped here rather than pushed through snt_core.
function(snt_app target main)
    add_executable(${target} ${main}
                   src/apps/paths.h src/apps/paths.cpp src/apps/pipeline_steps.h)
    target_link_libraries(${target} PRIVATE snt_core Boost::program_options)
endfunction()

snt_app(00_run_all           src/apps/run_all_main.cpp)
snt_app(01_extract           src/apps/extract_main.cpp)
snt_app(02_translate         src/apps/translate_main.cpp)
snt_app(03_find_narrative_cg src/apps/find_narrative_cg_main.cpp)

# --------------------------------------------------------------------
# Install: bin/ is what the user runs.  snt_core and every test binary stay
# out of it -- they are how the tools are built, not the tools.
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
