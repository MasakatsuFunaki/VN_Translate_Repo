# SYSTEM: End-to-end black-box test for the amakano2pe proxy DLL.
#
# Two binaries:
#   amakano2pe_driver: minimal host EXE; calls LoadLibraryW on the proxy
#                      so DllMain runs in a real process and writes the
#                      startup banner to proxy_log.txt.
#   amakano2pe_dll:    GoogleTest runner; spawns the driver, waits for exit,
#                      reads the log, and asserts the banner substring.

include(GoogleTest)

add_executable(amakano2pe_driver driver_exe.cpp)
target_link_libraries(amakano2pe_driver PRIVATE shell32)
target_compile_definitions(amakano2pe_driver PRIVATE
    _CRT_SECURE_NO_WARNINGS
)
add_dependencies(amakano2pe_driver proxy)

add_executable(amakano2pe_dll amakano2pe_dll.cpp)
target_link_libraries(amakano2pe_dll PRIVATE GTest::gtest GTest::gtest_main)
target_compile_definitions(amakano2pe_dll PRIVATE
    _CRT_SECURE_NO_WARNINGS
    PROXY_DLL_PATH="$<TARGET_FILE:proxy>"
    DRIVER_EXE_PATH="$<TARGET_FILE:amakano2pe_driver>"
    LOG_FILE_PATH="proxy_log.txt"
    # Match the unique prefix of the runtime startup log line without
    # parentheses that complicate compiler /D quote escaping.
    EXPECTED_BANNER="=== amakano2pe "
)
add_dependencies(amakano2pe_dll proxy amakano2pe_driver)
gtest_discover_tests(amakano2pe_dll PROPERTIES LABELS "SYSTEM")
