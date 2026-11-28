include(GoogleTest)

function(exm_test name)
    add_executable(${name} ${name}.cpp ${ARGN})
    target_link_libraries(${name} PRIVATE exm_core Boost::program_options shell32 GTest::gtest GTest::gtest_main)
    gtest_discover_tests(${name} PROPERTIES LABELS "UT")
endfunction()

function(exm_ct_test name)
    add_executable(${name} ${name}.cpp ${ARGN})
    target_link_libraries(${name} PRIVATE exm_core Boost::program_options GTest::gtest GTest::gtest_main)
    gtest_discover_tests(${name} PROPERTIES LABELS "CT")
endfunction()

function(exm_st_test name)
    add_executable(${name} ${name}.cpp ${ARGN})
    target_link_libraries(${name} PRIVATE exm_core Boost::program_options shell32 GTest::gtest GTest::gtest_main)
    gtest_discover_tests(${name} PROPERTIES LABELS "SYSTEM")
endfunction()

exm_test(ut_spt)
exm_test(ut_cache)
exm_test(ut_xtx)
exm_test(ut_menu_patcher)
exm_test(ut_paths ../src/apps/paths.cpp)

exm_ct_test(ct_menu_patcher)
exm_ct_test(ct_tsv_roundtrip ../../proxy_dll/src/translator_logic.cpp)
target_include_directories(ct_tsv_roundtrip PRIVATE ../../proxy_dll/src)

exm_st_test(st_live_game ../src/apps/paths.cpp ../../proxy_dll/src/translator_logic.cpp)
target_include_directories(st_live_game PRIVATE ../../proxy_dll/src)
exm_st_test(st_gemini_carbon ../src/apps/paths.cpp ../../proxy_dll/src/translator_logic.cpp)
target_include_directories(st_gemini_carbon PRIVATE ../../proxy_dll/src)
