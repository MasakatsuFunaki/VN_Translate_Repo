# GoogleTest suites: per-step unit tests plus format round-trip tests
# validated against real game data.

include(GoogleTest)

function(ama_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE ama_core GTest::gtest GTest::gtest_main)
    # Fixtures are real game data living in the source tree, so the tests need
    # an absolute anchor.  It stays PRIVATE to the test binaries -- these never
    # leave the build tree, unlike the installed apps.
    target_compile_definitions(${name} PRIVATE
        AMA_PROJECT_DIR="${AMA_PROJECT_DIR_ABS}")
    # One ctest entry per TEST() rather than per binary, so a failure names
    # the case directly.
    gtest_discover_tests(${name} PROPERTIES LABELS "UT")
endfunction()

ama_test(ut_cs2_archive)
ama_test(ut_extract)
ama_test(ut_translate)
ama_test(ut_build_tsv)
ama_test(ut_cp932)
ama_test(ut_extracted_integrity)
ama_test(ut_cache_io)
