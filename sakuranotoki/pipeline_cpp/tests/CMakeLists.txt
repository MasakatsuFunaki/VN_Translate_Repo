# GoogleTest suites split strictly across three test tiers:
#   UT     -- pure standalone unit tests (synthetic / temp-dir data only)
#   CT     -- clean-checkout component integration tests (temp-dir roundtrips)
#   SYSTEM -- system tests against an installed game archive or local corpus

include(GoogleTest)

function(snt_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE snt_core GTest::gtest GTest::gtest_main)
    gtest_discover_tests(${name} PROPERTIES LABELS "UT")
endfunction()

function(snt_ct_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE snt_core GTest::gtest GTest::gtest_main)
    gtest_discover_tests(${name} PROPERTIES LABELS "CT")
endfunction()

function(snt_st_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE snt_core GTest::gtest GTest::gtest_main)
    gtest_discover_tests(${name} PROPERTIES LABELS "SYSTEM")
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

add_executable(ut_paths ut_paths.cpp ../src/apps/paths.cpp)
target_link_libraries(ut_paths PRIVATE
    snt_core
    Boost::program_options
    GTest::gtest
    GTest::gtest_main
    shell32
)
gtest_discover_tests(ut_paths PROPERTIES LABELS "UT")

snt_ct_test(ct_translation_roundtrip)

snt_st_test(st_corpus)
target_compile_definitions(st_corpus PRIVATE
    SNT_PROJECT_DIR="${SNT_PROJECT_DIR_ABS}"
)
snt_st_test(st_pfs_archive)
