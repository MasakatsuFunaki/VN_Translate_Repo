cmake_minimum_required(VERSION 3.23)
project(AdieuPipeline LANGUAGES CXX)

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

if(NOT TARGET vn::llm_client)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../tools/llm_client
                     ${CMAKE_CURRENT_BINARY_DIR}/vn_llm_client
                     EXCLUDE_FROM_ALL)
endif()

# No project folder is compiled in: a build-time absolute path would ship this
# machine's directory layout inside every binary.  The apps walk up from their
# own executable to find it instead (src/apps/paths.cpp), and --dir overrides.

# --------------------------------------------------------------------
# adi_core: everything except the app mains, shared by apps and tests.
# --------------------------------------------------------------------
add_library(adi_core STATIC
    src/common/util.cpp
    src/common/util.h
    src/imaging/bgra_image.cpp
    src/imaging/bgra_image.h
    src/imaging/wic_image_io.cpp
    src/imaging/wic_image_io.h
    src/imaging/contact_sheet.cpp
    src/imaging/contact_sheet.h
    src/g00/g00_codec.cpp
    src/g00/g00_codec.h
    src/g00/gameexe_codec.cpp
    src/g00/gameexe_codec.h
    src/menu/core/sprite_canvas.cpp
    src/menu/core/sprite_canvas.h
    src/menu/core/menu_patch_container.cpp
    src/menu/core/menu_patch_container.h
    src/menu/domain_renderer.cpp
    src/menu/domain_renderer.h
    src/menu/gameexe_patch_rules.cpp
    src/menu/gameexe_patch_rules.h
    src/menu/render_system_menus.cpp
    src/menu/render_system_menus.h
    src/menu/render_mess_overlays.cpp
    src/menu/render_mess_overlays.h
    src/menu/render_cards_dialogs.cpp
    src/menu/render_cards_dialogs.h
    src/menu/render_supplementary.cpp
    src/menu/render_supplementary.h
    src/menu/adieu_menu_pipeline.cpp
    src/menu/adieu_menu_pipeline.h
    src/siglus/scene_pck.cpp
    src/siglus/scene_pck.h
    src/siglus/siglus_decoder.cpp
    src/siglus/siglus_decoder.h
    src/extract/extractor.cpp
    src/extract/extractor.h
    src/build_tsv/build_tsv.cpp
    src/build_tsv/build_tsv.h
    src/translate/glossary.cpp
    src/translate/glossary.h
    src/translate/gemini_client.cpp
    src/translate/gemini_client.h
    src/translate/translate_core.cpp
    src/translate/translate_core.h
)
target_include_directories(adi_core PUBLIC src)
target_compile_definitions(adi_core PUBLIC
    _CRT_SECURE_NO_WARNINGS
    _WIN32_WINNT=0x0601
)
target_link_libraries(adi_core PUBLIC
    vn::llm_client
    Boost::json
    Boost::regex
    # Span/header-only utilities only.  NOT the boost::boost aggregate -- that
    # pulls in every compiled Boost lib, including test_exec_monitor, whose
    # main() wants a test_main symbol the GoogleTest suites don't define.
    Boost::headers
)
target_link_libraries(adi_core PRIVATE
    gdi32
    ole32
    windowscodecs
)

# --------------------------------------------------------------------
# Pipeline executables, numbered by execution order.
#
# 00_run_all orchestrates extraction, translation, and game deployment.
# 01_extract decodes Scene.pck and attributes speakers.
# 02_build_tsv rebuilds translation_table.tsv from translated_text.json.
# 02_translate runs batched Gemini translation and builds the runtime table.
# 03_speaker_gate verifies speaker attribution and glossary coverage offline.
# 04_patch_menus builds menu_patch.bin or dumps UI assets for inspection.
# --------------------------------------------------------------------
# Only the app mains parse process command lines (src/apps/paths.h), so
# Program_options is scoped here rather than pushed through adi_core.
function(adi_app target main)
    add_executable(${target} ${main} src/apps/paths.h src/apps/paths.cpp)
    target_link_libraries(${target} PRIVATE adi_core Boost::program_options shell32)
endfunction()

adi_app(00_run_all       src/apps/run_all_main.cpp)
adi_app(01_extract       src/apps/extract_main.cpp)
adi_app(02_build_tsv     src/apps/build_tsv_main.cpp)
adi_app(02_translate     src/apps/translate_main.cpp)
adi_app(03_speaker_gate  src/apps/speaker_gate_main.cpp)
adi_app(04_patch_menus   src/apps/patch_menus_main.cpp)

# --------------------------------------------------------------------
# Install: the six executables a user runs, and nothing else.  adi_core and
# the test binaries exist to produce these and stay in the build tree.  Every
# dependency is a static library, so there is no runtime to carry.
# --------------------------------------------------------------------
install(TARGETS 00_run_all 01_extract 02_build_tsv 02_translate 03_speaker_gate 04_patch_menus
        RUNTIME DESTINATION bin)


# --------------------------------------------------------------------
# Tests
# --------------------------------------------------------------------
if(ENABLE_TESTS)
    find_package(GTest REQUIRED)
    enable_testing()
    add_subdirectory(tests)
endif()
