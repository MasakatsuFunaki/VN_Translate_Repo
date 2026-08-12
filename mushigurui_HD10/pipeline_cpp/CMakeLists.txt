cmake_minimum_required(VERSION 3.23)
project(MushiguruiPipeline LANGUAGES CXX)

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
# Narrative-CG step: FreeType does the text measurement/rasterisation and stb
# the image codecs.
find_package(freetype REQUIRED)
find_package(stb REQUIRED)

# No project folder is compiled in: a build-time absolute path would ship this
# machine's directory layout inside every binary.  The apps walk up from their
# own executable to find it instead (src/apps/paths.cpp), and --dir overrides.

# --------------------------------------------------------------------
# mgi_core: everything except the app mains, shared by apps and tests.
# --------------------------------------------------------------------
add_library(mgi_core STATIC
    src/common/util.cpp
    src/common/util.h
    src/spt/spt_script.cpp
    src/spt/spt_script.h
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
    src/xtx/xtx.cpp
    src/xtx/xtx.h
    src/xtx/xtx_tables.cpp
    src/gpk/gpk_archive.cpp
    src/gpk/gpk_archive.h
    src/cg/image.cpp
    src/cg/image.h
    src/cg/text_render.cpp
    src/cg/text_render.h
    src/cg/cg_pipeline.cpp
    src/cg/cg_pipeline.h
    src/choices/choice_render.cpp
    src/choices/choice_render.h
)
target_include_directories(mgi_core PUBLIC src)
target_compile_definitions(mgi_core PUBLIC
    _CRT_SECURE_NO_WARNINGS
    _WIN32_WINNT=0x0601
)
target_link_libraries(mgi_core PUBLIC
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
# playing.  03 to 05 are one-shot asset tools and deliberately NOT in
# 00_run_all: they touch the shipped config files, the CG archives and the
# choice screenshots, not the script text.
# --------------------------------------------------------------------
# Only the app mains parse a command line (src/apps/paths.h), so
# Program_options is scoped here rather than pushed through mgi_core.
function(mgi_app target main)
    add_executable(${target} ${main} src/apps/paths.h src/apps/paths.cpp)
    target_link_libraries(${target} PRIVATE mgi_core Boost::program_options)
endfunction()

mgi_app(00_run_all              src/apps/run_all_main.cpp)
mgi_app(01_extract              src/apps/extract_main.cpp)
mgi_app(02_translate            src/apps/translate_main.cpp)
mgi_app(03_translate_xtx        src/apps/translate_xtx_main.cpp)
mgi_app(04_find_narrative_cg    src/apps/find_narrative_cg_main.cpp)
mgi_app(05_replace_choice_text  src/apps/replace_choice_text_main.cpp)

# --------------------------------------------------------------------
# Install: the six executables a user runs, and nothing else.  mgi_core and
# the test binaries exist to produce these and stay in the build tree.  Every
# dependency is a static library, so there is no runtime to carry.
#
# The proxy DLL is installed by the other half into game/, never here: it
# carries a system DLL's name and is 32-bit, so a copy beside these x64
# executables is something the loader picks up and then refuses.
# --------------------------------------------------------------------
install(TARGETS 00_run_all 01_extract 02_translate
                03_translate_xtx 04_find_narrative_cg 05_replace_choice_text
        RUNTIME DESTINATION bin)


# --------------------------------------------------------------------
# Tests
# --------------------------------------------------------------------
if(ENABLE_TESTS)
    find_package(GTest REQUIRED)
    enable_testing()
    add_subdirectory(tests)
endif()
