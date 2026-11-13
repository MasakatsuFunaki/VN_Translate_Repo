# End-to-end black-box test for the FAVORITE proxy DLL.
#
# Two binaries:
#   favorite_driver -- minimal fake host EXE; calls LoadLibraryW on the proxy
#                      so DllMain ATTACH runs in a real process and writes the
#                      banner to proxy_log.txt.
#   favorite_dll    -- GoogleTest runner that spawns the driver, waits for
#                      exit, reads the log, and asserts the banner substring.

include(GoogleTest)

add_executable(favorite_driver driver_exe.cpp)
target_link_libraries(favorite_driver PRIVATE shell32)
target_compile_definitions(favorite_driver PRIVATE
    _CRT_SECURE_NO_WARNINGS
)
add_dependencies(favorite_driver proxy)

add_executable(favorite_dll favorite_dll.cpp)
target_link_libraries(favorite_dll PRIVATE GTest::gtest GTest::gtest_main)
target_compile_definitions(favorite_dll PRIVATE
    _CRT_SECURE_NO_WARNINGS
    PROXY_DLL_PATH="$<TARGET_FILE:proxy>"
    DRIVER_EXE_PATH="$<TARGET_FILE:favorite_driver>"
    LOG_FILE_PATH="proxy_log.txt"
    # Substring of "=== FAVORITE (FVP/Yoru-Tomosu) Proxy DLL loaded ===".
    # Parentheses and slash break quote escaping for /D, so match the unique
    # prefix only.
    EXPECTED_BANNER="=== FAVORITE "
)
add_dependencies(favorite_dll proxy favorite_driver)
gtest_discover_tests(favorite_dll PROPERTIES LABELS "SYSTEM")
