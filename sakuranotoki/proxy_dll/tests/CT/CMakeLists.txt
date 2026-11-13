include(GoogleTest)

add_executable(translation_pipeline_ct
    translation_pipeline_ct.cpp
    ../../src/translator_logic.cpp
)
target_include_directories(translation_pipeline_ct PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
)
target_compile_definitions(translation_pipeline_ct PRIVATE
    _CRT_SECURE_NO_WARNINGS
)
target_link_libraries(translation_pipeline_ct PRIVATE
    GTest::gtest
    GTest::gtest_main
)
gtest_discover_tests(translation_pipeline_ct PROPERTIES LABELS "CT")
