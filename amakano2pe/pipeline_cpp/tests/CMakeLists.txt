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
ama_test(ut_hg3_codec)
ama_test(ut_menu_patch_container)
ama_test(ut_sprite_canvas)

add_executable(ut_paths ut_paths.cpp ../src/apps/paths.cpp)
target_link_libraries(ut_paths PRIVATE ama_core Boost::program_options shell32 GTest::gtest GTest::gtest_main)
gtest_discover_tests(ut_paths PROPERTIES LABELS "UT")

ama_ct_test(ct_extracted_integrity)

add_executable(ct_menu_patcher
    ct_menu_patcher.cpp
    ../../proxy_dll/src/menu_patch_format.cpp
    ../../proxy_dll/src/menu_autopatch.cpp
    ../../proxy_dll/src/log.cpp
)
target_include_directories(ct_menu_patcher PRIVATE ../../proxy_dll/src)
target_link_libraries(ct_menu_patcher PRIVATE ama_core GTest::gtest GTest::gtest_main)
target_compile_definitions(ct_menu_patcher PRIVATE
    AMA_PROJECT_DIR="${AMA_PROJECT_DIR_ABS}"
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)
gtest_discover_tests(ct_menu_patcher PROPERTIES LABELS "CT")

ama_st_test(st_cs2_archive)

add_executable(st_menu_patcher
    st_menu_patcher.cpp
    ../../proxy_dll/src/menu_patch_format.cpp
    ../../proxy_dll/src/menu_autopatch.cpp
    ../../proxy_dll/src/log.cpp
)
target_include_directories(st_menu_patcher PRIVATE ../../proxy_dll/src)
target_link_libraries(st_menu_patcher PRIVATE ama_core GTest::gtest GTest::gtest_main)
target_compile_definitions(st_menu_patcher PRIVATE
    AMA_PROJECT_DIR="${AMA_PROJECT_DIR_ABS}"
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)
gtest_discover_tests(st_menu_patcher PROPERTIES LABELS "SYSTEM")
