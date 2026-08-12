# SYSTEM -- End-to-end black-box test for the EXTRAVAGANZA_CE proxy DLL.
#
# Two binaries:
#   extravaganza_ce_driver -- tiny fake host EXE; LoadLibrary's the proxy
#                              so DllMain runs in a real process (writes
#                              the banner to proxy_log.txt).
#   extravaganza_ce_dll    -- the gtest. Spawns the driver, waits, reads
#                              the log, asserts the banner substring.
#
# Per-game knobs are passed as compile-time -D defines so the gtest C++
# source stays identical across games and CMake stays the single point
# of variation.

include(GoogleTest)

# --- Fake host EXE ---------------------------------------------------
add_executable(extravaganza_ce_driver driver_exe.cpp)
target_compile_definitions(extravaganza_ce_driver PRIVATE
    _CRT_SECURE_NO_WARNINGS
)
add_dependencies(extravaganza_ce_driver proxy)

# --- gtest binary ----------------------------------------------------
add_executable(extravaganza_ce_dll extravaganza_ce_dll.cpp)
target_link_libraries(extravaganza_ce_dll PRIVATE GTest::gtest GTest::gtest_main)
target_compile_definitions(extravaganza_ce_dll PRIVATE
    _CRT_SECURE_NO_WARNINGS
    PROXY_DLL_PATH="$<TARGET_FILE:proxy>"
    DRIVER_EXE_PATH="$<TARGET_FILE:extravaganza_ce_driver>"
    LOG_FILE_PATH="proxy_log.txt"
    EXPECTED_BANNER="=== EXTRAVAGANZA Proxy DLL loaded ==="
)
add_dependencies(extravaganza_ce_dll proxy extravaganza_ce_driver)
gtest_discover_tests(extravaganza_ce_dll PROPERTIES LABELS "SYSTEM")
