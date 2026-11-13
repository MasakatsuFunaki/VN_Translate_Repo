# GoogleTest suites: hermetic unit tests (LABELS "UT") with zero repository-tree
# dependency, plus component tests (LABELS "CT") validated against on-disk
# artefacts in script_output/.

include(GoogleTest)

function(crc_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE crc_core GTest::gtest GTest::gtest_main)
    gtest_discover_tests(${name} PROPERTIES LABELS "UT")
endfunction()

crc_test(ut_sn_archive)
crc_test(ut_extract)
crc_test(ut_cache)
crc_test(ut_build_tsv)
crc_test(ut_cg)
crc_test(ut_cg_resume)
crc_test(ut_cp932)
crc_test(ut_cache_io)

add_executable(ut_paths ut_paths.cpp ../src/apps/paths.cpp)
target_link_libraries(ut_paths PRIVATE crc_core Boost::program_options GTest::gtest GTest::gtest_main shell32)
gtest_discover_tests(ut_paths PROPERTIES LABELS "UT")

add_executable(ct_corpus ct_corpus.cpp)
target_link_libraries(ct_corpus PRIVATE crc_core GTest::gtest GTest::gtest_main)
target_compile_definitions(ct_corpus PRIVATE
    CRC_PROJECT_DIR="${CRC_PROJECT_DIR}")
gtest_discover_tests(ct_corpus PROPERTIES LABELS "CT")

add_executable(st_cc_system st_cc_system.cpp)
target_link_libraries(st_cc_system PRIVATE crc_core GTest::gtest GTest::gtest_main)
target_compile_definitions(st_cc_system PRIVATE
    CRC_PROJECT_DIR="${CRC_PROJECT_DIR}")
gtest_discover_tests(st_cc_system PROPERTIES LABELS "SYSTEM")
