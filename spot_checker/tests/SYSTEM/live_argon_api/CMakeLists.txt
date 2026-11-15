include(GoogleTest)

add_executable(st_spot_check_live_api
    st_spot_check_live_api.cpp
)

target_include_directories(st_spot_check_live_api PRIVATE
    "${PROJECT_SOURCE_DIR}/tests/SYSTEM/common"
)

target_link_libraries(st_spot_check_live_api PRIVATE
    spot_checker_argon_client
    Boost::json
    GTest::gtest
    GTest::gtest_main
)

add_custom_command(TARGET st_spot_check_live_api POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/live_tone_of_novel.md"
        "$<TARGET_FILE_DIR:st_spot_check_live_api>/live_tone_of_novel.md"
)

gtest_discover_tests(st_spot_check_live_api
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    PROPERTIES LABELS "LIVE_API"
)
