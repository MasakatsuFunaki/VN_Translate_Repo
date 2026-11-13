include(GoogleTest)

add_executable(tsv_parser_ut
    tsv_parser_ut.cpp
    ../../src/translator_logic.cpp
)
target_include_directories(tsv_parser_ut PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
)
target_compile_definitions(tsv_parser_ut PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(tsv_parser_ut PRIVATE GTest::gtest GTest::gtest_main)
gtest_discover_tests(tsv_parser_ut PROPERTIES LABELS "UT")
