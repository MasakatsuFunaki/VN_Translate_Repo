cmake_minimum_required(VERSION 3.23)
project(FavoritePipeline LANGUAGES CXX)

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
find_package(ZLIB REQUIRED)
find_package(freetype REQUIRED)
find_package(stb REQUIRED)

# The tests -- and only the tests -- need an absolute anchor for the real
# game-data fixtures.  The apps must NOT get one: a compiled-in path ships
# this machine's directory layout inside every installed binary, so they find
# the project by walking up from their own executable (src/apps/paths.cpp).
get_filename_component(FAV_PROJECT_DIR_ABS "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# --------------------------------------------------------------------
# fav_core: everything except the app mains, shared by apps and tests.
# --------------------------------------------------------------------
add_library(fav_core STATIC
    src/common/util.cpp
    src/common/util.h
    src/fvp/fvp_hcb.cpp
    src/fvp/fvp_hcb.h
    src/fvp/fvp_graph.cpp
    src/fvp/fvp_graph.h
    src/extract/extractor.cpp
    src/extract/extractor.h
    src/build_tsv/build_tsv.cpp
    src/build_tsv/build_tsv.h
    src/translate/glossary.cpp
    src/translate/glossary.h
    src/translate/speaker_gate.cpp
    src/translate/speaker_gate.h
    src/translate/gemini_client.cpp
    src/translate/gemini_client.h
    src/translate/translate_core.cpp
    src/translate/translate_core.h
    src/ui/ui_patcher.cpp
    src/ui/ui_patcher.h
)
target_include_directories(fav_core PUBLIC src)
target_compile_definitions(fav_core PUBLIC
    _CRT_SECURE_NO_WARNINGS
    _WIN32_WINNT=0x0601
)
target_link_libraries(fav_core PUBLIC
    Boost::json
    Boost::regex
    # Beast/Asio only.  NOT the boost::boost aggregate -- that pulls in every
    # compiled Boost lib, including test_exec_monitor, whose main() wants a
    # test_main symbol the GoogleTest suites don't define.
    Boost::headers
    openssl::openssl
    ZLIB::ZLIB
    Freetype::Freetype
    stb::stb
    ws2_32
    crypt32
)

# --------------------------------------------------------------------
# Pipeline executables (numbered in run order).
# --------------------------------------------------------------------
# Only the app mains parse a command line (src/apps/paths.h), so
# Program_options is scoped here rather than pushed through fav_core.
function(fav_app target main)
    add_executable(${target} ${main} src/apps/paths.h src/apps/paths.cpp)
    target_link_libraries(${target} PRIVATE fav_core Boost::program_options)
endfunction()

fav_app(00_run_all  src/apps/run_all_main.cpp)
fav_app(01_extract  src/apps/extract_main.cpp)
fav_app(02_translate src/apps/translate_main.cpp)
fav_app(03_patch_ui src/apps/patch_ui_main.cpp)

# --------------------------------------------------------------------
# Install: the executables a user runs, and nothing else.  fav_core
# and the test binaries exist to produce these and stay in the build tree.
# Every dependency is a static library, so there is no runtime to carry.
# --------------------------------------------------------------------
install(TARGETS 00_run_all 01_extract 02_translate 03_patch_ui RUNTIME DESTINATION bin)


# --------------------------------------------------------------------
# Tests
# --------------------------------------------------------------------
if(ENABLE_TESTS)
    find_package(GTest REQUIRED)
    enable_testing()
    add_subdirectory(tests)
endif()
