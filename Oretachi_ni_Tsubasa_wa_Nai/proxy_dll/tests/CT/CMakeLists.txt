get_filename_component(ORETSUBA_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

add_executable(ct_tests translation_pipeline_ct.cpp)
target_compile_definitions(ct_tests PRIVATE
    ORETSUBA_ROOT_DIR="${ORETSUBA_ROOT_DIR}"
)
target_link_libraries(ct_tests PRIVATE translator_logic GTest::gtest GTest::gtest_main)

add_test(
    NAME CT_TranslationPipeline
    COMMAND ct_tests
    WORKING_DIRECTORY $<TARGET_FILE_DIR:ct_tests>
)
