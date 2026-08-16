# GoogleTest suites: the text-shaping rules each step depends on, plus
# format round-trip tests validated against real game data.

include(GoogleTest)

function(exc_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE exc_core GTest::gtest GTest::gtest_main)
    # One ctest entry per TEST() rather than per binary, so a failure names
    # the case directly.
    gtest_discover_tests(${name} PROPERTIES LABELS "UT")
endfunction()

exc_test(ut_spt_script)
exc_test(ut_translate)
exc_test(ut_repack)
exc_test(ut_charts)
exc_test(ut_cp932)
exc_test(ut_cache_io)
