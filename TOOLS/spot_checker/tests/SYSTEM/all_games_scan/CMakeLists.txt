include(GoogleTest)

add_executable(st_spot_check_all_games
    st_spot_check_all_games.cpp
)

target_include_directories(st_spot_check_all_games PRIVATE
    "${PROJECT_SOURCE_DIR}/tests/SYSTEM/common"
)

target_link_libraries(st_spot_check_all_games PRIVATE
    spot_checker_foundation
    Boost::json
    GTest::gtest
    GTest::gtest_main
)

gtest_discover_tests(st_spot_check_all_games
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    PROPERTIES LABELS "SYSTEM"
)
