# GoogleTest suites.  ut_golden_logs is the headline check: it rebuilds both
# analysis reports in-process and byte-compares them against the committed logs.

include(GoogleTest)

function(ksh_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE ksh_core GTest::gtest GTest::gtest_main)
    # The fixtures and golden logs live in the source tree, so the tests need
    # an absolute anchor.  It stays PRIVATE to the test binaries -- these never
    # leave the build tree, unlike the installed apps.
    target_compile_definitions(${name} PRIVATE
        KSH_PROJECT_DIR="${KSH_PROJECT_DIR_ABS}")
    gtest_discover_tests(${name} PROPERTIES LABELS "UT")
endfunction()

ksh_test(ut_pe_image)
ksh_test(ut_minidump)
ksh_test(ut_util)
ksh_test(ut_golden_logs)
ksh_test(ut_pak_builder)
