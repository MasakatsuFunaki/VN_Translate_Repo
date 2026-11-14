# SYSTEM -- End-to-end black-box and staged-table verification tests for Adieu.

include(GoogleTest)

add_executable(adieu_driver driver_exe.cpp)
target_compile_definitions(adieu_driver PRIVATE
    _CRT_SECURE_NO_WARNINGS
)
target_link_libraries(adieu_driver PRIVATE shell32)
add_dependencies(adieu_driver proxy)

add_executable(adieu_dll adieu_dll.cpp)
target_link_libraries(adieu_dll PRIVATE GTest::gtest GTest::gtest_main)
target_compile_definitions(adieu_dll PRIVATE
    _CRT_SECURE_NO_WARNINGS
    PROXY_DLL_PATH="$<TARGET_FILE:proxy>"
    DRIVER_EXE_PATH="$<TARGET_FILE:adieu_driver>"
    LOG_FILE_PATH="proxy_log.txt"
    EXPECTED_BANNER="=== Adieu "
)
add_dependencies(adieu_dll proxy adieu_driver)
gtest_discover_tests(adieu_dll PROPERTIES LABELS "SYSTEM")

add_executable(translation_table_st
    translation_table_st.cpp
    $<TARGET_OBJECTS:translator_logic>
)
target_link_libraries(translation_table_st PRIVATE
    GTest::gtest
    GTest::gtest_main
)
target_include_directories(translation_table_st PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_definitions(translation_table_st PRIVATE
    _CRT_SECURE_NO_WARNINGS
    GAME_TSV_PATH="${CMAKE_SOURCE_DIR}/../build/install/game/translation_table.tsv"
)
gtest_discover_tests(translation_table_st PROPERTIES LABELS "SYSTEM")
