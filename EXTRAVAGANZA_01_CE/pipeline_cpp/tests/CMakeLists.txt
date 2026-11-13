# GoogleTest suites separated into standalone unit tests (UT), repository
# component tests (CT), and live-game system tests (SYSTEM).

include(GoogleTest)

function(exc_labeled_test name label)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE exc_core GTest::gtest GTest::gtest_main)
    gtest_discover_tests(${name} PROPERTIES LABELS "${label}")
endfunction()

function(exc_test name)
    exc_labeled_test(${name} "UT")
endfunction()

function(exc_ct_test name)
    exc_labeled_test(${name} "CT")
endfunction()

function(exc_st_test name)
    exc_labeled_test(${name} "SYSTEM")
endfunction()

exc_test(ut_spt_script)
exc_test(ut_cache)
exc_test(ut_repack)
exc_test(ut_charts)
exc_test(ut_cp932)
exc_test(ut_cache_io)
exc_test(ut_menu_patcher)

add_executable(ut_paths ut_paths.cpp ../src/apps/paths.cpp)
target_link_libraries(ut_paths PRIVATE exc_core Boost::program_options shell32 GTest::gtest GTest::gtest_main)
gtest_discover_tests(ut_paths PROPERTIES LABELS "UT")

exc_ct_test(ct_menu_patcher)
exc_st_test(st_menu_patcher)
