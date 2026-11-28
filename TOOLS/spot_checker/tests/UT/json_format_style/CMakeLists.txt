include(GoogleTest)

add_executable(ut_json_format_style ut_json_format_style.cpp)
target_link_libraries(ut_json_format_style PRIVATE
    spot_checker_foundation
    GTest::gtest
    GTest::gtest_main
)
gtest_discover_tests(ut_json_format_style
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:ut_json_format_style>"
    PROPERTIES LABELS "UT"
)
