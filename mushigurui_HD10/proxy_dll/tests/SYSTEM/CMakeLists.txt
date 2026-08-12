# SYSTEM -- End-to-end black-box test for the mushigurui_HD10 proxy DLL.
#
# Two binaries:
#   mushigurui_hd10_driver -- tiny fake host EXE; LoadLibrary's the proxy
#                              so DllMain runs in a real process (writes
#                              the banner to proxy_log.txt).
#   mushigurui_hd10_dll    -- the gtest. Spawns the driver, waits, reads
#                              the log, asserts the banner substring.

include(GoogleTest)

# --- Fake host EXE ---------------------------------------------------
add_executable(mushigurui_hd10_driver driver_exe.cpp)
target_compile_definitions(mushigurui_hd10_driver PRIVATE
    _CRT_SECURE_NO_WARNINGS
)
add_dependencies(mushigurui_hd10_driver proxy)

# --- gtest binary ----------------------------------------------------
add_executable(mushigurui_hd10_dll mushigurui_hd10_dll.cpp)
target_link_libraries(mushigurui_hd10_dll PRIVATE GTest::gtest GTest::gtest_main)
target_compile_definitions(mushigurui_hd10_dll PRIVATE
    _CRT_SECURE_NO_WARNINGS
    PROXY_DLL_PATH="$<TARGET_FILE:proxy>"
    DRIVER_EXE_PATH="$<TARGET_FILE:mushigurui_hd10_driver>"
    LOG_FILE_PATH="proxy_log.txt"
    EXPECTED_BANNER="=== mushigurui_HD10 Proxy DLL loaded ==="
)
add_dependencies(mushigurui_hd10_dll proxy mushigurui_hd10_driver)
gtest_discover_tests(mushigurui_hd10_dll PROPERTIES LABELS "SYSTEM")
