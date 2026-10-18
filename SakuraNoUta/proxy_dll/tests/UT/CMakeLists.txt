# UT — Unit Tests for SakuraNoUta.
#
# One executable per class/module. Naming: <classname>_ut.cpp →
# target name <classname>_ut.

include(GoogleTest)

#   add_ut_target(<name> [extra object libraries...])
# Every suite gets translator_logic's objects; a suite that exercises another
# module names it as an extra argument.
function(add_ut_target name)
    add_executable(${name} ${name}.cpp $<TARGET_OBJECTS:translator_logic> ${ARGN})
    target_link_libraries(${name} PRIVATE GTest::gtest GTest::gtest_main)
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(${name} PRIVATE _CRT_SECURE_NO_WARNINGS)
    gtest_discover_tests(${name} PROPERTIES LABELS "UT")
endfunction()

add_ut_target(tsv_parser_ut)
add_ut_target(cp932_ut)
add_ut_target(word_wrap_ut)
add_ut_target(text_fold_ut)
add_ut_target(substitution_ut)
add_ut_target(text_display_abi_ut $<TARGET_OBJECTS:text_display_abi>)
