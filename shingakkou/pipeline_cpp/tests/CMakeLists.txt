# GoogleTest suites: the pipeline's behavioural rules plus format round-trip
# tests over fixtures built in memory, so they run without the game installed.

include(GoogleTest)

function(shin_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE shin_core GTest::gtest GTest::gtest_main)
    # Fixtures are real game data living in the source tree, so the tests need
    # an absolute anchor.  It stays PRIVATE to the test binaries -- these never
    # leave the build tree, unlike the installed apps.
    target_compile_definitions(${name} PRIVATE
        SHIN_PROJECT_DIR="${SHIN_PROJECT_DIR_ABS}")
    # One ctest entry per TEST() rather than per binary, so a failure names
    # the case directly.
    gtest_discover_tests(${name} PROPERTIES LABELS "UT")
endfunction()

shin_test(ut_ddp_archive)
shin_test(ut_extract)
shin_test(ut_translate)
shin_test(ut_build_tsv)
shin_test(ut_cg)
