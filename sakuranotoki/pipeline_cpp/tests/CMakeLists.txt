# GoogleTest suites, mirrored from the retired pytest suites under
# implementation/tests plus format round-trip tests the Python side
# validated against real game data.

include(GoogleTest)

function(snt_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE snt_core GTest::gtest GTest::gtest_main)
    # Fixtures are real game data living in the source tree, so the tests need
    # an absolute anchor.  It stays PRIVATE to the test binaries -- these never
    # leave the build tree, unlike the installed apps.
    target_compile_definitions(${name} PRIVATE
        SNT_PROJECT_DIR="${SNT_PROJECT_DIR_ABS}")
    # One ctest entry per TEST() rather than per binary, so a failure names
    # the case directly.
    gtest_discover_tests(${name} PROPERTIES LABELS "UT")
endfunction()

snt_test(ut_pfs_archive)
snt_test(ut_extract)
snt_test(ut_translate)
snt_test(ut_speaker_gate)
snt_test(ut_build_tsv)
snt_test(ut_cg_resume)
snt_test(ut_cp932)
snt_test(ut_cache_io)
snt_test(ut_coverage)
