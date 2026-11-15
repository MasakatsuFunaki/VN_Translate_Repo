include(GoogleTest)

add_executable(ut_win32_cli_env ut_win32_cli_env.cpp)
target_link_libraries(ut_win32_cli_env PRIVATE
    spot_checker_foundation
    GTest::gtest
    GTest::gtest_main
)
gtest_discover_tests(ut_win32_cli_env
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:ut_win32_cli_env>"
    PROPERTIES LABELS "UT"
)
