include(GoogleTest)

function(exm_test name)
    add_executable(${name} ${name}.cpp ${ARGN})
    target_link_libraries(${name} PRIVATE exm_core Boost::program_options GTest::gtest GTest::gtest_main)
    gtest_discover_tests(${name} PROPERTIES LABELS "UT")
endfunction()

exm_test(ut_encoding)
exm_test(ut_spt_script)
exm_test(ut_glossary)
exm_test(ut_xtx)
exm_test(ut_build_tsv)
exm_test(ut_menu_patcher)
