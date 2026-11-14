include(GoogleTest)

add_executable(menu_autopatch_ct
    menu_autopatch_ct.cpp
    $<TARGET_OBJECTS:patcher_logic>
)
target_link_libraries(menu_autopatch_ct PRIVATE GTest::gtest GTest::gtest_main)
target_include_directories(menu_autopatch_ct PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(menu_autopatch_ct PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
)
gtest_discover_tests(menu_autopatch_ct PROPERTIES LABELS "CT")
