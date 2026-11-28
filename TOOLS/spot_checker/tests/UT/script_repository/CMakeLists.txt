include(GoogleTest)

add_executable(ut_script_repository ut_script_repository.cpp)
target_link_libraries(ut_script_repository PRIVATE
    spot_checker_foundation
    GTest::gtest
    GTest::gtest_main
)
gtest_discover_tests(ut_script_repository
    WORKING_DIRECTORY "$<TARGET_FILE_DIR:ut_script_repository>"
    PROPERTIES LABELS "UT"
)
