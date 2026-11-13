# SYSTEM -- End-to-end black-box and binary-dump verification tests for CROSS_CHANNEL.

include(GoogleTest)

add_executable(cross_channel_driver driver_exe.cpp)
target_compile_definitions(cross_channel_driver PRIVATE
    _CRT_SECURE_NO_WARNINGS
)
target_link_libraries(cross_channel_driver PRIVATE shell32)
add_dependencies(cross_channel_driver proxy)

add_executable(cross_channel_dll cross_channel_dll.cpp)
target_link_libraries(cross_channel_dll PRIVATE GTest::gtest GTest::gtest_main)
target_compile_definitions(cross_channel_dll PRIVATE
    _CRT_SECURE_NO_WARNINGS
    PROXY_DLL_PATH="$<TARGET_FILE:proxy>"
    DRIVER_EXE_PATH="$<TARGET_FILE:cross_channel_driver>"
    LOG_FILE_PATH="proxy_log.txt"
    EXPECTED_BANNER="=== CROSS-CHANNEL "
)
add_dependencies(cross_channel_dll proxy cross_channel_driver)
gtest_discover_tests(cross_channel_dll PROPERTIES LABELS "SYSTEM")

add_executable(hook_sites_st hook_sites_st.cpp)
target_link_libraries(hook_sites_st PRIVATE GTest::gtest GTest::gtest_main)
target_compile_definitions(hook_sites_st PRIVATE
    _CRT_SECURE_NO_WARNINGS
    CC_DUMPED_EXE_PATH="${CMAKE_SOURCE_DIR}/../analysys/cc_dumped.exe"
)
gtest_discover_tests(hook_sites_st PROPERTIES LABELS "SYSTEM")
