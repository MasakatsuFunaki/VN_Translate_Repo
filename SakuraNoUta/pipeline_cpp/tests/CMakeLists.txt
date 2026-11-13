# GoogleTest suites separated into pure standalone UT, corpus CT, and real-game SYSTEM tiers.

include(GoogleTest)

function(snu_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE snu_core GTest::gtest GTest::gtest_main)
    gtest_discover_tests(${name} PROPERTIES LABELS "UT")
endfunction()

function(snu_ct_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE snu_core GTest::gtest GTest::gtest_main)
    target_compile_definitions(${name} PRIVATE
        SNU_PROJECT_DIR="${SNU_PROJECT_DIR_ABS}")
    gtest_discover_tests(${name} PROPERTIES LABELS "CT")
endfunction()

function(snu_st_test name)
    add_executable(${name} ${name}.cpp ../src/apps/paths.cpp)
    target_link_libraries(${name} PRIVATE snu_core Boost::program_options shell32 GTest::gtest GTest::gtest_main)
    target_compile_definitions(${name} PRIVATE
        SNU_PROJECT_DIR="${SNU_PROJECT_DIR_ABS}")
    gtest_discover_tests(${name} PROPERTIES LABELS "SYSTEM")
endfunction()

snu_test(ut_bgi_archive)
snu_test(ut_cbg)
snu_test(ut_cg_resume)
snu_test(ut_extract)
snu_test(ut_cache)
snu_test(ut_build_tsv)
snu_test(ut_cache_io)

add_executable(ut_paths ut_paths.cpp ../src/apps/paths.cpp)
target_link_libraries(ut_paths PRIVATE snu_core Boost::program_options shell32 GTest::gtest GTest::gtest_main)
gtest_discover_tests(ut_paths PROPERTIES LABELS "UT")

snu_ct_test(ct_corpus)
snu_st_test(st_bgi_system)
