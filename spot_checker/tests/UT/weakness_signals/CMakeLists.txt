include(GoogleTest)

add_executable(ut_weakness_signals ut_weakness_signals.cpp)
target_link_libraries(ut_weakness_signals PRIVATE
    spot_checker_engine
    GTest::gtest
    GTest::gtest_main
)
gtest_discover_tests(ut_weakness_signals
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:ut_weakness_signals>"
    PROPERTIES LABELS "UT"
)
