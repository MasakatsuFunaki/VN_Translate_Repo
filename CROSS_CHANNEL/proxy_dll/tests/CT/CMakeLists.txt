# Component tests for CROSS_CHANNEL proxy_dll.

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
target_compile_definitions(translation_pipeline_ct PRIVATE
    _CRT_SECURE_NO_WARNINGS
    GAME_TSV_PATH="${CMAKE_SOURCE_DIR}/../build/install/game/translations.tsv"
)
gtest_discover_tests(translation_pipeline_ct PROPERTIES LABELS "CT")
