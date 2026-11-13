add_executable(ut_tests tsv_parser_ut.cpp)
target_link_libraries(ut_tests PRIVATE translator_logic GTest::gtest GTest::gtest_main)

add_test(
    NAME UT_TsvParser
    COMMAND ut_tests
    WORKING_DIRECTORY $<TARGET_FILE_DIR:ut_tests>
)
set_tests_properties(UT_TsvParser PROPERTIES LABELS "UT")
