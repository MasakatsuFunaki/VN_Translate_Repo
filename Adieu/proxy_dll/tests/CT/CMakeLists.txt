# Component tests for Adieu proxy_dll.

include(GoogleTest)

add_executable(translation_pipeline_ct
    translation_pipeline_ct.cpp
    $<TARGET_OBJECTS:translator_logic>
)
target_link_libraries(translation_pipeline_ct PRIVATE
    GTest::gtest
    GTest::gtest_main
)
target_include_directories(translation_pipeline_ct PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_definitions(translation_pipeline_ct PRIVATE
    _CRT_SECURE_NO_WARNINGS
)
gtest_discover_tests(translation_pipeline_ct PROPERTIES LABELS "CT")

add_executable(menu_autopatch_ct
    menu_autopatch_ct.cpp
    $<TARGET_OBJECTS:menu_autopatch>
    $<TARGET_OBJECTS:menu_patch_format>
    $<TARGET_OBJECTS:proxy_log>
)
target_link_libraries(menu_autopatch_ct PRIVATE
    GTest::gtest
    GTest::gtest_main
)
target_include_directories(menu_autopatch_ct PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_definitions(menu_autopatch_ct PRIVATE
    _CRT_SECURE_NO_WARNINGS
    ADIEU_SOURCE_DIR="${CMAKE_SOURCE_DIR}/.."
)
gtest_discover_tests(menu_autopatch_ct PROPERTIES LABELS "CT")
