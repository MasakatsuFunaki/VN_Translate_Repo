include(GoogleTest)

add_executable(ut_hotspot_detector ut_hotspot_detector.cpp)
target_link_libraries(ut_hotspot_detector PRIVATE
    spot_checker_engine
    GTest::gtest
    GTest::gtest_main
)
gtest_discover_tests(ut_hotspot_detector
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:ut_hotspot_detector>"
    PROPERTIES LABELS "UT"
)
