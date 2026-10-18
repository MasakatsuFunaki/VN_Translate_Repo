# CT — Component Tests for SakuraNoUta.
#
# One executable per component: <componentname>_ct.cpp → <componentname>_ct.

include(GoogleTest)

add_executable(translation_pipeline_ct
    translation_pipeline_ct.cpp
    $<TARGET_OBJECTS:translator_logic>
)
target_link_libraries(translation_pipeline_ct PRIVATE
    GTest::gtest
    GTest::gtest_main
)
target_include_directories(translation_pipeline_ct PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
# The table under test is the one the DLL ships with, resolved relative to
# this repository. An absolute path would only exist on the machine it was
# typed on, and the tests would then GTEST_SKIP() everywhere else while ctest
# still reported a full pass.
get_filename_component(GAME_TSV_FILE
    "${CMAKE_SOURCE_DIR}/../translations.tsv" ABSOLUTE)

target_compile_definitions(translation_pipeline_ct PRIVATE
    _CRT_SECURE_NO_WARNINGS
    GAME_TSV_PATH="${GAME_TSV_FILE}"
)
gtest_discover_tests(translation_pipeline_ct PROPERTIES LABELS "CT")

# Optional CT: hook_sites_ct.cpp (game-specific binary contract). Built only
# if the file is present — usually a hand-written game-specific test that
# validates RVAs / opcode bytes / imm32 globals in the game's dumped EXE.
# CC_DUMPED_EXE_PATH is wired so the test SKIPs cleanly when the dump is
# absent (large files not always present in fresh checkouts).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/hook_sites_ct.cpp")
    add_executable(hook_sites_ct hook_sites_ct.cpp)
    target_link_libraries(hook_sites_ct PRIVATE GTest::gtest GTest::gtest_main)
    target_compile_definitions(hook_sites_ct PRIVATE
        _CRT_SECURE_NO_WARNINGS
        CC_DUMPED_EXE_PATH="${CMAKE_SOURCE_DIR}/../analysys/cc_dumped.exe"
    )
    gtest_discover_tests(hook_sites_ct PROPERTIES LABELS "CT")
endif()
