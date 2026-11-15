include(GoogleTest)

add_executable(ut_utf8_text ut_utf8_text.cpp)
target_link_libraries(ut_utf8_text PRIVATE
    spot_checker_foundation
    GTest::gtest
    GTest::gtest_main
)
gtest_discover_tests(ut_utf8_text
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:ut_utf8_text>"
    PROPERTIES LABELS "UT"
)
