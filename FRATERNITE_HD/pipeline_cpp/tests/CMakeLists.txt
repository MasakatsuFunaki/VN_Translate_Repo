include(GoogleTest)

function(frat_test name)
    add_executable(${name} ${name}.cpp test_support.h)
    target_link_libraries(${name} PRIVATE frat_core GTest::gtest GTest::gtest_main)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    gtest_discover_tests(${name} PROPERTIES LABELS "UT")
endfunction()

function(frat_ct_test name)
    add_executable(${name} ${name}.cpp test_support.h)
    target_link_libraries(${name} PRIVATE frat_core GTest::gtest GTest::gtest_main)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(${name} PRIVATE
        FRAT_PROJECT_DIR="${FRAT_PROJECT_DIR_FWD}")
    gtest_discover_tests(${name} PROPERTIES LABELS "CT")
endfunction()

function(frat_st_test name)
    add_executable(${name} ${name}.cpp test_support.h)
    target_link_libraries(${name} PRIVATE frat_core GTest::gtest GTest::gtest_main)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(${name} PRIVATE
        FRAT_PROJECT_DIR="${FRAT_PROJECT_DIR_FWD}")
    gtest_discover_tests(${name} PROPERTIES LABELS "SYSTEM")
endfunction()

frat_test(ut_cp932)
frat_test(ut_cp932_scan)
frat_test(ut_ypf_archive)
frat_test(ut_extract)
frat_test(ut_cache)
frat_test(ut_build_tsv)
frat_test(ut_speaker_gate)
frat_test(ut_cache_io)
frat_test(ut_menu_patcher)

add_executable(ut_paths ut_paths.cpp ../src/apps/paths.cpp)
target_link_libraries(ut_paths PRIVATE frat_core Boost::program_options GTest::gtest GTest::gtest_main shell32)
gtest_discover_tests(ut_paths PROPERTIES LABELS "UT")

frat_ct_test(ct_frat_artifacts)
frat_st_test(st_menu_patcher)
