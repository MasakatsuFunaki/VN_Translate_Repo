include(GoogleTest)

function(oretsuba_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE oretsuba_core GTest::gtest GTest::gtest_main)
    target_compile_definitions(${name} PRIVATE
        ORETSUBA_PROJECT_DIR="${ORETSUBA_PROJECT_DIR_ABS}")
    gtest_discover_tests(${name} PROPERTIES LABELS "UT")
endfunction()

oretsuba_test(ut_lpk_archive)
oretsuba_test(ut_sob_parser)
oretsuba_test(ut_extract)
oretsuba_test(ut_translate)
oretsuba_test(ut_build_tsv)
