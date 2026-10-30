# GoogleTest suites: pure-helper unit tests plus format round-trip tests
# validated against real game data.

include(GoogleTest)

function(snu_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE snu_core GTest::gtest GTest::gtest_main)
    # Fixtures are real game data living in the source tree, so the tests need
    # an absolute anchor.  It stays PRIVATE to the test binaries -- these never
    # leave the build tree, unlike the installed apps.
    target_compile_definitions(${name} PRIVATE
        SNU_PROJECT_DIR="${SNU_PROJECT_DIR_ABS}")
    # One ctest entry per TEST() rather than per binary, so a failure names
    # the case directly.
    gtest_discover_tests(${name} PROPERTIES LABELS "UT")
endfunction()

snu_test(ut_bgi_archive)
snu_test(ut_cbg)
snu_test(ut_cg_resume)
snu_test(ut_extract)
snu_test(ut_cache)
snu_test(ut_build_tsv)
snu_test(ut_cache_io)
