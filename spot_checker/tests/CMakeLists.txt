include(GoogleTest)

add_executable(ut_spot_checker ut_spot_checker.cpp)
target_link_libraries(ut_spot_checker PRIVATE
    spot_checker_core
    GTest::gtest
    GTest::gtest_main
)
gtest_discover_tests(ut_spot_checker PROPERTIES LABELS "UT")
