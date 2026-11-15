# UT — Unit Tests for CROSS_CHANNEL.
#
# One executable per class/module. Naming: <classname>_ut.cpp →
# target name <classname>_ut.

include(GoogleTest)

function(add_ut_target name)
    add_executable(${name} ${name}.cpp ${ARGN} $<TARGET_OBJECTS:translator_logic>)
    target_link_libraries(${name} PRIVATE GTest::gtest GTest::gtest_main)
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(${name} PRIVATE _CRT_SECURE_NO_WARNINGS)
    gtest_discover_tests(${name} PROPERTIES LABELS "UT")
endfunction()

add_ut_target(tsv_parser_ut)
add_ut_target(cp932_ut)
add_ut_target(patcher_ut)
add_ut_target(fallback_lookup_ut)
add_ut_target(bytecode_menus_ut ${CMAKE_SOURCE_DIR}/src/bytecode_hooks.cpp)
add_ut_target(chrome_text_logic_ut)
add_ut_target(menu_patch_format_ut)
add_ut_target(menu_autopatch_ut ${CMAKE_SOURCE_DIR}/src/menu_autopatch.cpp)
