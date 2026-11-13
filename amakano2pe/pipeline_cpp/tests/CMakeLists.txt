# GoogleTest suites organized by test tier:
#   UT     -- pure standalone synthetic unit tests
#   CT     -- component tests over repository artifacts
#   SYSTEM -- system tests against the installed game archive

include(GoogleTest)

function(ama_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE ama_core GTest::gtest GTest::gtest_main)
    gtest_discover_tests(${name} PROPERTIES LABELS "UT")
endfunction()

function(ama_ct_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE ama_core GTest::gtest GTest::gtest_main)
    target_compile_definitions(${name} PRIVATE
        AMA_PROJECT_DIR="${AMA_PROJECT_DIR_ABS}")
    gtest_discover_tests(${name} PROPERTIES LABELS "CT")
endfunction()

function(ama_st_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE ama_core GTest::gtest GTest::gtest_main)
    target_compile_definitions(${name} PRIVATE
        AMA_PROJECT_DIR="${AMA_PROJECT_DIR_ABS}")
    gtest_discover_tests(${name} PROPERTIES LABELS "SYSTEM")
endfunction()

ama_test(ut_cs2_archive)
ama_test(ut_extract)
ama_test(ut_cache)
ama_test(ut_build_tsv)
ama_test(ut_cp932)
ama_test(ut_cache_io)

add_executable(ut_paths ut_paths.cpp ../src/apps/paths.cpp)
target_link_libraries(ut_paths PRIVATE ama_core Boost::program_options shell32 GTest::gtest GTest::gtest_main)
gtest_discover_tests(ut_paths PROPERTIES LABELS "UT")

ama_ct_test(ct_extracted_integrity)
ama_st_test(st_cs2_archive)
