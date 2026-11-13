# GoogleTest suites separated into standalone unit tests (UT), repository
# component tests (CT), and live-game system tests (SYSTEM).

include(GoogleTest)

function(exm_labeled_test name label)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE exm_core GTest::gtest GTest::gtest_main)
    gtest_discover_tests(${name} PROPERTIES LABELS "${label}")
endfunction()

function(exm_test name)
    exm_labeled_test(${name} "UT")
endfunction()

function(exm_ct_test name)
    exm_labeled_test(${name} "CT")
endfunction()

function(exm_st_test name)
    exm_labeled_test(${name} "SYSTEM")
endfunction()

exm_test(ut_spt_script)
exm_test(ut_cache)
exm_test(ut_build_tsv)
exm_test(ut_cp932)
exm_test(ut_cache_io)
exm_test(ut_menu_patcher)

add_executable(ut_paths ut_paths.cpp ../src/apps/paths.cpp)
target_link_libraries(ut_paths PRIVATE exm_core Boost::program_options shell32 GTest::gtest GTest::gtest_main)
gtest_discover_tests(ut_paths PROPERTIES LABELS "UT")

exm_ct_test(ct_menu_patcher)
exm_st_test(st_menu_patcher)
