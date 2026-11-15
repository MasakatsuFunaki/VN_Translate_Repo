include(GoogleTest)

add_executable(ut_spot_check_checkpoint ut_spot_check_checkpoint.cpp)
target_link_libraries(ut_spot_check_checkpoint PRIVATE
    spot_checker_engine
    GTest::gtest
    GTest::gtest_main
)
gtest_discover_tests(ut_spot_check_checkpoint
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:ut_spot_check_checkpoint>"
    PROPERTIES LABELS "UT"
)
