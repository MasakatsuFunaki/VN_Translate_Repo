# UT — Unit Tests for amakano2pe.
#
# One executable per class/module. Naming: <classname>_ut.cpp →
# target name <classname>_ut.

include(GoogleTest)

function(add_ut_target name)
    add_executable(${name} ${name}.cpp $<TARGET_OBJECTS:translator_logic>)
    target_link_libraries(${name} PRIVATE GTest::gtest GTest::gtest_main)
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(${name} PRIVATE _CRT_SECURE_NO_WARNINGS)
    gtest_discover_tests(${name} PROPERTIES LABELS "UT")
endfunction()

add_ut_target(tsv_parser_ut)
add_ut_target(ui_text_lookup_ut)
add_ut_target(chrome_text_logic_ut)

add_executable(win32_chrome_ut
    win32_chrome_ut.cpp
    $<TARGET_OBJECTS:translator_logic>
    $<TARGET_OBJECTS:proxy_log>
    $<TARGET_OBJECTS:win32_chrome>
)
target_link_libraries(win32_chrome_ut PRIVATE
    GTest::gtest
    GTest::gtest_main
    user32
)
target_include_directories(win32_chrome_ut PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(win32_chrome_ut PRIVATE _CRT_SECURE_NO_WARNINGS)
gtest_discover_tests(win32_chrome_ut PROPERTIES LABELS "UT")

add_executable(menu_patch_format_ut
    menu_patch_format_ut.cpp
    $<TARGET_OBJECTS:menu_patch_format>
)
target_link_libraries(menu_patch_format_ut PRIVATE
    GTest::gtest
    GTest::gtest_main
)
target_include_directories(menu_patch_format_ut PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(menu_patch_format_ut PRIVATE _CRT_SECURE_NO_WARNINGS)
gtest_discover_tests(menu_patch_format_ut PROPERTIES LABELS "UT")

add_executable(menu_autopatch_ut
    menu_autopatch_ut.cpp
    $<TARGET_OBJECTS:menu_patch_format>
    $<TARGET_OBJECTS:proxy_log>
    $<TARGET_OBJECTS:menu_autopatch>
)
target_link_libraries(menu_autopatch_ut PRIVATE
    GTest::gtest
    GTest::gtest_main
)
target_include_directories(menu_autopatch_ut PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(menu_autopatch_ut PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
)
gtest_discover_tests(menu_autopatch_ut PROPERTIES LABELS "UT")

add_executable(dialogue_session_ut
    dialogue_session_ut.cpp
    $<TARGET_OBJECTS:translator_logic>
    $<TARGET_OBJECTS:gdi_text_measurer>
)
target_link_libraries(dialogue_session_ut PRIVATE
    GTest::gtest
    GTest::gtest_main
    gdi32
)
target_include_directories(dialogue_session_ut PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(dialogue_session_ut PRIVATE _CRT_SECURE_NO_WARNINGS)
gtest_discover_tests(dialogue_session_ut PROPERTIES LABELS "UT")
