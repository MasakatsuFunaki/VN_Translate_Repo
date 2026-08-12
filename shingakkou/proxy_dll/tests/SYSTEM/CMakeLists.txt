# SYSTEM -- End-to-end black-box test for the shingakkou proxy DLL.
#
# Two binaries:
#   shingakkou_driver -- tiny fake host EXE; LoadLibrary's the proxy
#                         so DllMain runs in a real process (writes
#                         the banner to proxy_log.txt).
#   shingakkou_dll    -- the gtest. Spawns the driver, waits, reads
#                         the log, asserts the banner substring.

include(GoogleTest)

# --- Fake host EXE ---------------------------------------------------
add_executable(shingakkou_driver driver_exe.cpp)
target_compile_definitions(shingakkou_driver PRIVATE
    _CRT_SECURE_NO_WARNINGS
)
add_dependencies(shingakkou_driver proxy)

# --- gtest binary ----------------------------------------------------
add_executable(shingakkou_dll shingakkou_dll.cpp)
target_link_libraries(shingakkou_dll PRIVATE GTest::gtest GTest::gtest_main)
target_compile_definitions(shingakkou_dll PRIVATE
    _CRT_SECURE_NO_WARNINGS
    PROXY_DLL_PATH="$<TARGET_FILE:proxy>"
    DRIVER_EXE_PATH="$<TARGET_FILE:shingakkou_driver>"
    LOG_FILE_PATH="proxy_log.txt"
    EXPECTED_BANNER="=== Shingakkou Proxy DLL loaded ==="
)
add_dependencies(shingakkou_dll proxy shingakkou_driver)
gtest_discover_tests(shingakkou_dll PROPERTIES LABELS "SYSTEM")
