# UT -- Unit Tests for Adieu proxy_dll.
#
# One executable per class/module. Naming: <classname>_ut.cpp ->
# target name <classname>_ut.

include(GoogleTest)

function(add_ut_target name)
    add_executable(${name} ${name}.cpp ${ARGN})
    target_link_libraries(${name} PRIVATE GTest::gtest GTest::gtest_main)
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(${name} PRIVATE _CRT_SECURE_NO_WARNINGS)
    gtest_discover_tests(${name} PROPERTIES LABELS "UT")
endfunction()

add_ut_target(tsv_parser_ut $<TARGET_OBJECTS:translator_logic>)
add_ut_target(nvl_wrap_ut $<TARGET_OBJECTS:translator_logic>)
add_ut_target(chrome_text_logic_ut $<TARGET_OBJECTS:chrome_text_logic>)
add_ut_target(menu_patch_format_ut $<TARGET_OBJECTS:menu_patch_format>)
add_ut_target(menu_autopatch_ut
    $<TARGET_OBJECTS:menu_autopatch>
    $<TARGET_OBJECTS:menu_patch_format>
    $<TARGET_OBJECTS:proxy_log>
)
