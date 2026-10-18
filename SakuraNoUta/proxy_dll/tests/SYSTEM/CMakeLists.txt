# SYSTEM -- End-to-end black-box test for the SakuraNoUta proxy DLL.
#
# Two binaries:
#   sakuranouta_driver -- tiny fake host EXE; LoadLibrary's the proxy
#                          so DllMain runs in a real process (writes
#                          the banner to proxy_log.txt).
#   sakuranouta_dll    -- the gtest. Spawns the driver, waits, reads
#                          the log, asserts the banner substring.

include(GoogleTest)

# --- Fake host EXE ---------------------------------------------------
add_executable(sakuranouta_driver driver_exe.cpp)
target_compile_definitions(sakuranouta_driver PRIVATE
    _CRT_SECURE_NO_WARNINGS
)
add_dependencies(sakuranouta_driver proxy)

# --- gtest binary ----------------------------------------------------
add_executable(sakuranouta_dll sakuranouta_dll.cpp)
target_link_libraries(sakuranouta_dll PRIVATE GTest::gtest GTest::gtest_main)
target_compile_definitions(sakuranouta_dll PRIVATE
    _CRT_SECURE_NO_WARNINGS
    PROXY_DLL_PATH="$<TARGET_FILE:proxy>"
    DRIVER_EXE_PATH="$<TARGET_FILE:sakuranouta_driver>"
    # The DLL writes its log beside the host executable, wherever the
    # process was launched from -- so the log to read sits by the driver.
    LOG_FILE_PATH="$<TARGET_FILE_DIR:sakuranouta_driver>/proxy_log.txt"
    # NOTE: substring of "=== SakuraNoUta (BGI / Buriko) Proxy DLL loaded
    # ===". Parens + slash break MSBuild quote escaping for /D, so match
    # the unique prefix only.
    EXPECTED_BANNER="=== SakuraNoUta "
)
add_dependencies(sakuranouta_dll proxy sakuranouta_driver)
gtest_discover_tests(sakuranouta_dll PROPERTIES LABELS "SYSTEM")
