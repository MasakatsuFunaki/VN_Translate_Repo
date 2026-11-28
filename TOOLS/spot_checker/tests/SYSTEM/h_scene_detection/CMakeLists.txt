include(GoogleTest)

add_executable(st_h_scene_detection
    st_h_scene_detection.cpp
)

target_include_directories(st_h_scene_detection PRIVATE
    "${PROJECT_SOURCE_DIR}/tests/SYSTEM/common"
)

target_link_libraries(st_h_scene_detection PRIVATE
    spot_checker_foundation
    Boost::json
    GTest::gtest
    GTest::gtest_main
)

gtest_discover_tests(st_h_scene_detection
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    PROPERTIES LABELS "SYSTEM"
)
