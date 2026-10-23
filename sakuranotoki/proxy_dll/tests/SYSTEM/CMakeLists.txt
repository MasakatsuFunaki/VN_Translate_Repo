# SYSTEM -- End-to-end black-box test for the sakuranotoki proxy DLL.
#
# Two binaries:
#   sakuranotoki_driver -- tiny fake host EXE; LoadLibrary's the proxy
#                           so DllMain runs in a real process (writes
#                           the log banner next to the DLL).
#   sakuranotoki_dll    -- the gtest. Spawns the driver, waits, reads
#                           the log, asserts the banner substring.
#
# sakuranotoki diverges from the other games in two ways:
#   - DLL target name is `version_proxy` (not `proxy`); OUTPUT_NAME is
#     version.dll (mimics the launcher's IAT entry for version.dll).
#   - log_init builds an absolute log path from GetModuleFileNameA
#     (so the log lands next to the DLL, not in the cwd). The gtest
#     therefore reads from $<TARGET_FILE_DIR:version_proxy>/<filename>
#     instead of a relative path.

include(GoogleTest)

# --- Fake host EXE ---------------------------------------------------
add_executable(sakuranotoki_driver driver_exe.cpp)
target_compile_definitions(sakuranotoki_driver PRIVATE
    _CRT_SECURE_NO_WARNINGS
)
add_dependencies(sakuranotoki_driver version_proxy)

# --- gtest binary ----------------------------------------------------
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
