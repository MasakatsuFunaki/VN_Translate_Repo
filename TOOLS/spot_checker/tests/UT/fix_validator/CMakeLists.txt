include(GoogleTest)

add_executable(ut_fix_validator ut_fix_validator.cpp)
target_link_libraries(ut_fix_validator PRIVATE
    spot_checker_engine
    GTest::gtest
    GTest::gtest_main
)
gtest_discover_tests(ut_fix_validator
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:ut_fix_validator>"
    PROPERTIES LABELS "UT"
)
