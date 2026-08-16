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
mgi_test(ut_translate)
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
