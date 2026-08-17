include(GoogleTest)

function(frat_test name)
    add_executable(${name} ${name}.cpp test_support.h)
    target_link_libraries(${name} PRIVATE frat_core GTest::gtest GTest::gtest_main)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    # PRIVATE: test binaries never leave the build tree.
    target_compile_definitions(${name} PRIVATE
        FRAT_PROJECT_DIR="${FRAT_PROJECT_DIR_ABS}")
    gtest_discover_tests(${name} PROPERTIES LABELS "UT")
endfunction()

frat_test(ut_cp932)
frat_test(ut_cp932_scan)
frat_test(ut_ypf_archive)
frat_test(ut_extract)
frat_test(ut_translate)
frat_test(ut_build_tsv)
frat_test(ut_speaker_gate)
frat_test(ut_cache_io)
