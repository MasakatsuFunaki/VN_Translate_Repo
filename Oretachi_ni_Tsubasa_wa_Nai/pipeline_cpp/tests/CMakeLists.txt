include(GoogleTest)

function(oretsuba_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE oretsuba_core GTest::gtest GTest::gtest_main)
    gtest_discover_tests(${name} PROPERTIES LABELS "UT")
endfunction()

oretsuba_test(ut_lpk_archive)
oretsuba_test(ut_sob_parser)
oretsuba_test(ut_extract)
oretsuba_test(ut_translate)
oretsuba_test(ut_build_tsv)
oretsuba_test(ut_paths)

add_executable(ct_translate_corpus ct_translate_corpus.cpp)
target_link_libraries(ct_translate_corpus PRIVATE oretsuba_core GTest::gtest GTest::gtest_main)
target_compile_definitions(ct_translate_corpus PRIVATE
    ORETSUBA_PROJECT_DIR="${ORETSUBA_PROJECT_DIR}")
gtest_discover_tests(ct_translate_corpus PROPERTIES LABELS "CT")

add_executable(st_oretsuba_system st_oretsuba_system.cpp)
target_link_libraries(st_oretsuba_system PRIVATE oretsuba_core GTest::gtest GTest::gtest_main)
target_compile_definitions(st_oretsuba_system PRIVATE
    ORETSUBA_PROJECT_DIR="${ORETSUBA_PROJECT_DIR}")
gtest_discover_tests(st_oretsuba_system PROPERTIES LABELS "SYSTEM")
