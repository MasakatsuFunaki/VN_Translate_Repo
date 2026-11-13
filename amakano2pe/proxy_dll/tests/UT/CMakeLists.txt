# UT — Unit Tests for amakano2pe.
#
# One executable per class/module. Naming: <classname>_ut.cpp →
# target name <classname>_ut.

include(GoogleTest)

function(add_ut_target name)
    add_executable(${name} ${name}.cpp $<TARGET_OBJECTS:translator_logic>)
    target_link_libraries(${name} PRIVATE GTest::gtest GTest::gtest_main)
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(${name} PRIVATE _CRT_SECURE_NO_WARNINGS)
    gtest_discover_tests(${name} PROPERTIES LABELS "UT")
endfunction()

add_ut_target(tsv_parser_ut)

add_executable(dialogue_session_ut
    dialogue_session_ut.cpp
    $<TARGET_OBJECTS:translator_logic>
    $<TARGET_OBJECTS:gdi_text_measurer>
)
target_link_libraries(dialogue_session_ut PRIVATE
    GTest::gtest
    GTest::gtest_main
    gdi32
)
target_include_directories(dialogue_session_ut PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(dialogue_session_ut PRIVATE _CRT_SECURE_NO_WARNINGS)
gtest_discover_tests(dialogue_session_ut PROPERTIES LABELS "UT")
