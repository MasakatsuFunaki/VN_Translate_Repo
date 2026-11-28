include(GoogleTest)

add_executable(ut_argon_wire_codec ut_argon_wire_codec.cpp)
target_link_libraries(ut_argon_wire_codec PRIVATE
    spot_checker_argon_client
    GTest::gtest
    GTest::gtest_main
)
gtest_discover_tests(ut_argon_wire_codec
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:ut_argon_wire_codec>"
    PROPERTIES LABELS "UT"
)
