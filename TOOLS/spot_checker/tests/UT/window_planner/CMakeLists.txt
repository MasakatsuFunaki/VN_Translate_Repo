include(GoogleTest)

add_executable(ut_window_planner ut_window_planner.cpp)
target_link_libraries(ut_window_planner PRIVATE
    spot_checker_engine
    GTest::gtest
    GTest::gtest_main
)
gtest_discover_tests(ut_window_planner
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:ut_window_planner>"
    PROPERTIES LABELS "UT"
)
