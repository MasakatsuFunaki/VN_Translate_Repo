include(GoogleTest)

function(fav_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE fav_core GTest::gtest GTest::gtest_main)
    gtest_discover_tests(${name} PROPERTIES LABELS "UT")
endfunction()

function(fav_ct_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE fav_core GTest::gtest GTest::gtest_main)
    target_compile_definitions(${name} PRIVATE
        FAV_PROJECT_DIR="${FAV_PROJECT_DIR_ABS}")
    gtest_discover_tests(${name} PROPERTIES LABELS "CT")
endfunction()

function(fav_st_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE fav_core GTest::gtest GTest::gtest_main)
    target_compile_definitions(${name} PRIVATE
        FAV_PROJECT_DIR="${FAV_PROJECT_DIR_ABS}")
    gtest_discover_tests(${name} PROPERTIES LABELS "SYSTEM")
endfunction()

fav_test(ut_fvp_hcb)
fav_test(ut_extract)
fav_test(ut_translate)
fav_test(ut_gemini_client)
fav_test(ut_build_tsv)
fav_test(ut_cp932)
fav_test(ut_cache_io)
fav_test(ut_fvp_graph)

add_executable(ut_paths ut_paths.cpp ../src/apps/paths.cpp)
target_link_libraries(ut_paths PRIVATE fav_core Boost::program_options shell32 GTest::gtest GTest::gtest_main)
gtest_discover_tests(ut_paths PROPERTIES LABELS "UT")

fav_ct_test(ct_extract_corpus)

add_executable(ct_translate_pipeline
    ct_translate_pipeline.cpp
    ../../proxy_dll/src/translator_logic.cpp
)
target_include_directories(ct_translate_pipeline PRIVATE ../../proxy_dll/src)
target_link_libraries(ct_translate_pipeline PRIVATE fav_core GTest::gtest GTest::gtest_main)
target_compile_definitions(ct_translate_pipeline PRIVATE
    FAV_PROJECT_DIR="${FAV_PROJECT_DIR_ABS}")
gtest_discover_tests(ct_translate_pipeline PROPERTIES LABELS "CT")

fav_st_test(st_fvp_system)
add_dependencies(st_fvp_system 01_extract 02_translate)
