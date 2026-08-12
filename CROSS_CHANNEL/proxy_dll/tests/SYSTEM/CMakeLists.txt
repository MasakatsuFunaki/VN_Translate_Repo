# SYSTEM -- End-to-end black-box test for the CROSS_CHANNEL proxy DLL.
#
# Two binaries:
#   cross_channel_driver -- tiny fake host EXE; LoadLibrary's the proxy
#                            so DllMain runs in a real process (writes
#                            the banner to proxy_log.txt).
#   cross_channel_dll    -- the gtest. Spawns the driver, waits, reads
#                            the log, asserts the banner substring.

include(GoogleTest)

# --- Fake host EXE ---------------------------------------------------
add_executable(cross_channel_driver driver_exe.cpp)
target_compile_definitions(cross_channel_driver PRIVATE
    _CRT_SECURE_NO_WARNINGS
)
add_dependencies(cross_channel_driver proxy)

# --- gtest binary ----------------------------------------------------
add_executable(cross_channel_dll cross_channel_dll.cpp)
target_link_libraries(cross_channel_dll PRIVATE GTest::gtest GTest::gtest_main)
target_compile_definitions(cross_channel_dll PRIVATE
    _CRT_SECURE_NO_WARNINGS
    PROXY_DLL_PATH="$<TARGET_FILE:proxy>"
    DRIVER_EXE_PATH="$<TARGET_FILE:cross_channel_driver>"
    LOG_FILE_PATH="proxy_log.txt"
    # NOTE: substring of "=== CROSS-CHANNEL (cc.exe / WillPlus) Proxy DLL
    # loaded ===". Parens + slash break MSBuild quote escaping for /D
    # preprocessor args, so we match the unique prefix only.
    EXPECTED_BANNER="=== CROSS-CHANNEL "
)
add_dependencies(cross_channel_dll proxy cross_channel_driver)
gtest_discover_tests(cross_channel_dll PROPERTIES LABELS "SYSTEM")
