include(GoogleTest)

add_executable(ut_vn_line_syntax ut_vn_line_syntax.cpp)
target_link_libraries(ut_vn_line_syntax PRIVATE
    spot_checker_foundation
    GTest::gtest
    GTest::gtest_main
)
gtest_discover_tests(ut_vn_line_syntax
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:ut_vn_line_syntax>"
    PROPERTIES LABELS "UT"
)
