# GoogleTest suites.  ut_* targets are pure synthetic unit tests with zero
# checkout-tree dependency; st_alpharom_samples runs against the committed
# AlphaROM sample executable, crash dump, and golden logs.

include(GoogleTest)

function(ksh_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE ksh_core GTest::gtest GTest::gtest_main)
    gtest_discover_tests(${name} PROPERTIES LABELS "UT")
endfunction()

ksh_test(ut_pe_image)
ksh_test(ut_minidump)
ksh_test(ut_util)
ksh_test(ut_pak_builder)

add_executable(ut_paths ut_paths.cpp ../src/apps/paths.cpp)
target_link_libraries(ut_paths PRIVATE ksh_core Boost::program_options shell32 GTest::gtest GTest::gtest_main)
gtest_discover_tests(ut_paths PROPERTIES LABELS "UT")

add_executable(st_alpharom_samples st_alpharom_samples.cpp)
target_link_libraries(st_alpharom_samples PRIVATE ksh_core GTest::gtest GTest::gtest_main)
target_compile_definitions(st_alpharom_samples PRIVATE
    KSH_PROJECT_DIR="${KSH_PROJECT_DIR}")
gtest_discover_tests(st_alpharom_samples PROPERTIES LABELS "SYSTEM")
