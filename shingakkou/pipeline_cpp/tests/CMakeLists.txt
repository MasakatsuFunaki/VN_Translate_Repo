# GoogleTest suites split across three tiers:
#   UT      — pure standalone unit tests using only in-memory or temp fixtures
#   CT      — component tests over committed repository artifacts
#   SYSTEM  — live-game archive tests gated on VN_GAME_DIR

include(GoogleTest)

function(shin_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE shin_core GTest::gtest GTest::gtest_main)
    gtest_discover_tests(${name} PROPERTIES LABELS "UT")
endfunction()

function(shin_ct_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE shin_core GTest::gtest GTest::gtest_main)
    target_compile_definitions(${name} PRIVATE
        SHIN_PROJECT_DIR="${SHIN_PROJECT_DIR_FWD}")
    gtest_discover_tests(${name} PROPERTIES LABELS "CT")
endfunction()

function(shin_st_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE shin_core GTest::gtest GTest::gtest_main)
    target_compile_definitions(${name} PRIVATE
        SHIN_PROJECT_DIR="${SHIN_PROJECT_DIR_FWD}")
    gtest_discover_tests(${name} PROPERTIES LABELS "SYSTEM")
endfunction()

shin_test(ut_ddp_archive)
shin_test(ut_extract)
shin_test(ut_cache)
shin_test(ut_build_tsv)
shin_test(ut_cg)
shin_test(ut_cg_resume)
shin_test(ut_cache_io)
shin_test(ut_menu_patcher)

add_executable(ut_paths ut_paths.cpp ../src/apps/paths.cpp)
target_link_libraries(ut_paths PRIVATE shin_core Boost::program_options shell32 GTest::gtest GTest::gtest_main)
gtest_discover_tests(ut_paths PROPERTIES LABELS "UT")

shin_ct_test(ct_shin_artifacts)
shin_st_test(st_shin_live_game)
