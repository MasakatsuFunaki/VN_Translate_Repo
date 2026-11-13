include(GoogleTest)

add_executable(ut_spot_checker ut_spot_checker.cpp)
target_link_libraries(ut_spot_checker PRIVATE
    spot_checker_core
    GTest::gtest
    GTest::gtest_main
)
gtest_discover_tests(ut_spot_checker PROPERTIES LABELS "UT")

add_executable(st_spot_checker_live_api st_spot_checker_live_api.cpp)
target_link_libraries(st_spot_checker_live_api PRIVATE
    spot_checker_core
    GTest::gtest
    GTest::gtest_main
)
gtest_discover_tests(st_spot_checker_live_api PROPERTIES LABELS "SYSTEM")
