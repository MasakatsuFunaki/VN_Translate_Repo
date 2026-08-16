# SYSTEM -- End-to-end black-box test for the amakano2pe proxy DLL.
#
# Two binaries:
#   amakano2pe_driver -- tiny fake host EXE; LoadLibrary's the proxy
#                         so DllMain runs in a real process (writes
#                         the banner to proxy_log.txt).
#   amakano2pe_dll    -- the gtest. Spawns the driver, waits, reads
#                         the log, asserts the banner substring.

include(GoogleTest)

# --- Fake host EXE ---------------------------------------------------
add_executable(amakano2pe_driver driver_exe.cpp)
target_compile_definitions(amakano2pe_driver PRIVATE
    _CRT_SECURE_NO_WARNINGS
)
add_dependencies(amakano2pe_driver proxy)

# --- gtest binary ----------------------------------------------------
add_executable(amakano2pe_dll amakano2pe_dll.cpp)
target_link_libraries(amakano2pe_dll PRIVATE GTest::gtest GTest::gtest_main)
target_compile_definitions(amakano2pe_dll PRIVATE
    _CRT_SECURE_NO_WARNINGS
    PROXY_DLL_PATH="$<TARGET_FILE:proxy>"
    DRIVER_EXE_PATH="$<TARGET_FILE:amakano2pe_driver>"
    LOG_FILE_PATH="proxy_log.txt"
    # NOTE: substring of "=== amakano2pe (CatSystem2) Proxy DLL loaded
    # ===". Parens break MSBuild quote escaping for /D, so match the
    # unique prefix only.
    EXPECTED_BANNER="=== amakano2pe "
)
add_dependencies(amakano2pe_dll proxy amakano2pe_driver)
gtest_discover_tests(amakano2pe_dll PROPERTIES LABELS "SYSTEM")
