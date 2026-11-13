# End-to-end black-box and local-artifact system tests for sakuranotoki proxy_dll:
#   sakuranotoki_driver     -- minimal host EXE that calls LoadLibraryW on the proxy
#   sakuranotoki_dll        -- GoogleTest runner that spawns the driver and checks
#                              the proxy log banner
#   translation_pipeline_st -- verifies loading local translations.tsv when present

include(GoogleTest)

get_filename_component(SNT_PROXY_PROJECT_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

add_executable(sakuranotoki_driver driver_exe.cpp)
target_link_libraries(sakuranotoki_driver PRIVATE shell32)
target_compile_definitions(sakuranotoki_driver PRIVATE
    _CRT_SECURE_NO_WARNINGS
)
add_dependencies(sakuranotoki_driver version_proxy)

add_executable(sakuranotoki_dll sakuranotoki_dll.cpp)
target_link_libraries(sakuranotoki_dll PRIVATE GTest::gtest GTest::gtest_main)
target_compile_definitions(sakuranotoki_dll PRIVATE
    _CRT_SECURE_NO_WARNINGS
    PROXY_DLL_PATH="$<TARGET_FILE:version_proxy>"
    DRIVER_EXE_PATH="$<TARGET_FILE:sakuranotoki_driver>"
    LOG_FILE_PATH="$<TARGET_FILE_DIR:version_proxy>/sakuranotoki_proxy.log"
    EXPECTED_BANNER="sakuranotoki version.dll proxy log opened"
)
add_dependencies(sakuranotoki_dll version_proxy sakuranotoki_driver)
gtest_discover_tests(sakuranotoki_dll PROPERTIES LABELS "SYSTEM")

add_executable(translation_pipeline_st
    translation_pipeline_st.cpp
    ../../src/translator_logic.cpp
)
target_include_directories(translation_pipeline_st PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
)
target_compile_definitions(translation_pipeline_st PRIVATE
    _CRT_SECURE_NO_WARNINGS
    VN_PROJECT_ROOT_DIR="${SNT_PROXY_PROJECT_ROOT}"
)
target_link_libraries(translation_pipeline_st PRIVATE
    GTest::gtest
    GTest::gtest_main
)
gtest_discover_tests(translation_pipeline_st PROPERTIES LABELS "SYSTEM")
