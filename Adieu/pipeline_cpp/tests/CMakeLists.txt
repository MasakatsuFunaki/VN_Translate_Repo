# GoogleTest suites.  The cases pin the behaviours a plausible-looking rewrite
# gets wrong silently -- decoder edge cases, key order and output encoding.

include(GoogleTest)

function(adi_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE adi_core GTest::gtest GTest::gtest_main)
    # One ctest entry per TEST() rather than per binary, so a failure names
    # the case directly.
    gtest_discover_tests(${name} PROPERTIES LABELS "UT")
endfunction()

function(adi_ct_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE adi_core GTest::gtest GTest::gtest_main)
    gtest_discover_tests(${name} PROPERTIES LABELS "CT")
endfunction()

function(adi_st_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE adi_core GTest::gtest GTest::gtest_main)
    gtest_discover_tests(${name} PROPERTIES LABELS "SYSTEM")
endfunction()

adi_test(ut_utf16)
adi_test(ut_siglus_decoder)
adi_test(ut_scene_pck)
adi_test(ut_extract)
adi_test(ut_gemini_client)
adi_test(ut_translate)
adi_test(ut_build_tsv)
adi_test(ut_cache_io)
adi_test(ut_g00_codec)
adi_test(ut_gameexe_codec)
adi_test(ut_sprite_canvas)
adi_test(ut_menu_patch_container)

add_executable(ut_paths ut_paths.cpp ../src/apps/paths.cpp)
target_link_libraries(ut_paths PRIVATE adi_core Boost::program_options shell32 GTest::gtest GTest::gtest_main)
gtest_discover_tests(ut_paths PROPERTIES LABELS "UT")

adi_ct_test(ct_menu_patcher)
target_compile_definitions(ct_menu_patcher PRIVATE
    ADIEU_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../.."
)
add_executable(ct_translate ct_translate.cpp ../src/apps/paths.cpp)
target_link_libraries(ct_translate PRIVATE adi_core Boost::program_options shell32 GTest::gtest GTest::gtest_main)
gtest_discover_tests(ct_translate PROPERTIES LABELS "CT")
add_dependencies(ct_translate 02_translate)

adi_st_test(st_siglus_system)
adi_st_test(st_gameexe_roundtrip)
adi_st_test(st_menu_patcher)
adi_st_test(st_gemini_translate)
add_dependencies(st_gemini_translate 02_translate)
target_compile_definitions(st_menu_patcher PRIVATE
    ADIEU_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../.."
)
