include(GoogleTest)

add_executable(ut_prompt_codec ut_prompt_codec.cpp)
target_link_libraries(ut_prompt_codec PRIVATE
    spot_checker_engine
    GTest::gtest
    GTest::gtest_main
)
gtest_discover_tests(ut_prompt_codec
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:ut_prompt_codec>"
    PROPERTIES LABELS "UT"
)
