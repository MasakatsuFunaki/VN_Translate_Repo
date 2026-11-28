include(GoogleTest)

add_executable(ct_spot_checker
    ct_spot_checker.cpp
)

target_include_directories(ct_spot_checker PRIVATE
    "${PROJECT_SOURCE_DIR}/tests/COMPONENT/common"
)

target_link_libraries(ct_spot_checker PRIVATE
    spot_checker_engine
    Boost::json
    GTest::gtest
    GTest::gtest_main
)

add_custom_command(TARGET ct_spot_checker POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/tone_of_novel.md"
        "$<TARGET_FILE_DIR:ct_spot_checker>/tone_of_novel.md"
)

gtest_discover_tests(ct_spot_checker
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    PROPERTIES LABELS "CT"
)
