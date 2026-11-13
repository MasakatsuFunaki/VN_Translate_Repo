# GoogleTest suites: the pipeline's classification/translation rules plus
# format round-trip tests validated against real game data.

include(GoogleTest)

function(mgi_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE mgi_core GTest::gtest GTest::gtest_main)
    # One ctest entry per TEST() rather than per binary, so a failure names
    # the case directly.
    gtest_discover_tests(${name} PROPERTIES LABELS "UT")
endfunction()

mgi_test(ut_spt_script)
mgi_test(ut_cache)
mgi_test(ut_build_tsv)
mgi_test(ut_cp932)
mgi_test(ut_json_float)
mgi_test(ut_gpk_archive)
mgi_test(ut_cg_image)
mgi_test(ut_cg_render)
mgi_test(ut_cg_parse)
mgi_test(ut_cg_resume)
mgi_test(ut_choices)
mgi_test(ut_cache_io)
mgi_test(ut_menu_patcher)

add_executable(ct_menu_patcher ct_menu_patcher.cpp)
target_link_libraries(ct_menu_patcher PRIVATE mgi_core GTest::gtest GTest::gtest_main)
gtest_discover_tests(ct_menu_patcher PROPERTIES LABELS "CT")

add_executable(st_menu_patcher st_menu_patcher.cpp)
target_link_libraries(st_menu_patcher PRIVATE mgi_core GTest::gtest GTest::gtest_main)
gtest_discover_tests(st_menu_patcher PROPERTIES LABELS "SYSTEM")

add_executable(ut_paths ut_paths.cpp ../src/apps/paths.cpp)
target_link_libraries(ut_paths PRIVATE mgi_core Boost::program_options GTest::gtest GTest::gtest_main shell32)
gtest_discover_tests(ut_paths PROPERTIES LABELS "UT")
