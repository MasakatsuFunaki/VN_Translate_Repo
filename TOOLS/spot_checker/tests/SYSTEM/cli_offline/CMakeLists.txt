include(GoogleTest)

add_executable(st_spot_check_cli
    st_spot_check_cli.cpp
)

target_include_directories(st_spot_check_cli PRIVATE
    "${PROJECT_SOURCE_DIR}/tests/SYSTEM/common"
)

target_link_libraries(st_spot_check_cli PRIVATE
    spot_checker_foundation
    Boost::json
    GTest::gtest
    GTest::gtest_main
)

add_custom_command(TARGET st_spot_check_cli POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/tone_of_novel.md"
        "$<TARGET_FILE_DIR:st_spot_check_cli>/tone_of_novel.md"
)

gtest_discover_tests(st_spot_check_cli
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    PROPERTIES LABELS "SYSTEM"
)
