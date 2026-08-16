# GoogleTest suites: unit tests for the SPT reader, the translator helpers and
# the TSV builder, plus format round-trip tests pinned against real game data.

include(GoogleTest)

function(exm_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE exm_core GTest::gtest GTest::gtest_main)
    # One ctest entry per TEST() rather than per binary, so a failure names
    # the case directly.
    gtest_discover_tests(${name} PROPERTIES LABELS "UT")
endfunction()

exm_test(ut_spt_script)
exm_test(ut_translate)
exm_test(ut_build_tsv)
exm_test(ut_cp932)
exm_test(ut_cache_io)
