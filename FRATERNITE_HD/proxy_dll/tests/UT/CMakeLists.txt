# UT — Unit Tests.

include(GoogleTest)

function(add_ut_target name)
    add_executable(${name} ${name}.cpp $<TARGET_OBJECTS:translator_logic>)
    target_link_libraries(${name} PRIVATE GTest::gtest GTest::gtest_main)
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(${name} PRIVATE _CRT_SECURE_NO_WARNINGS)
    gtest_discover_tests(${name} PROPERTIES LABELS "UT")
endfunction()

add_ut_target(tsv_parser_ut)
