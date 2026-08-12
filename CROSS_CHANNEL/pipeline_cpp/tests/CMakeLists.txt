# GoogleTest suites: unit tests for each pipeline stage plus format
# round-trip tests validated against the committed real-data artefacts in
# script_output/.

include(GoogleTest)

function(crc_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE crc_core GTest::gtest GTest::gtest_main)
    # Fixtures are real game data living in the source tree, so the tests need
    # an absolute anchor.  It stays PRIVATE to the test binaries -- these never
    # leave the build tree, unlike the installed apps.
    target_compile_definitions(${name} PRIVATE
        CRC_PROJECT_DIR="${CRC_PROJECT_DIR_ABS}")
    # One ctest entry per TEST() rather than per binary, so a failure names
    # the case directly.
    gtest_discover_tests(${name} PROPERTIES LABELS "UT")
endfunction()

crc_test(ut_sn_archive)
crc_test(ut_extract)
crc_test(ut_translate)
crc_test(ut_build_tsv)
crc_test(ut_cg)
crc_test(ut_cp932)
