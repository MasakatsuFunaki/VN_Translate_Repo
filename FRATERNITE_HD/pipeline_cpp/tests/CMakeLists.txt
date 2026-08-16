# GoogleTest suites: one per pipeline module, plus the digest-pinned cases
# that lock the on-disk output formats.

include(GoogleTest)

function(frat_test name)
    add_executable(${name} ${name}.cpp test_support.h)
    target_link_libraries(${name} PRIVATE frat_core GTest::gtest GTest::gtest_main)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    # Fixtures are real game data living in the source tree, so the tests need
    # an absolute anchor.  It stays PRIVATE to the test binaries -- these never
    # leave the build tree, unlike the installed apps.
    target_compile_definitions(${name} PRIVATE
        FRAT_PROJECT_DIR="${FRAT_PROJECT_DIR_ABS}")
    # One ctest entry per TEST() rather than per binary, so a failure names
    # the case directly.
    gtest_discover_tests(${name} PROPERTIES LABELS "UT")
endfunction()

frat_test(ut_cp932)
frat_test(ut_cp932_scan)
frat_test(ut_ypf_archive)
frat_test(ut_extract)
frat_test(ut_translate)
frat_test(ut_build_tsv)
frat_test(ut_speaker_gate)
frat_test(ut_cache_io)
