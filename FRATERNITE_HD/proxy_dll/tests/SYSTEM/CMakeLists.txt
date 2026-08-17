# SYSTEM -- black-box DLL test.
# fraternite_hd_driver: fake host that LoadLibrary's the proxy.
# fraternite_hd_dll: gtest that spawns the driver and checks the log.

include(GoogleTest)

# --- Fake host EXE ---------------------------------------------------
add_executable(fraternite_hd_driver driver_exe.cpp)
target_compile_definitions(fraternite_hd_driver PRIVATE
    _CRT_SECURE_NO_WARNINGS
)
add_dependencies(fraternite_hd_driver proxy)

# --- gtest binary ----------------------------------------------------
add_executable(fraternite_hd_dll fraternite_hd_dll.cpp)
target_link_libraries(fraternite_hd_dll PRIVATE GTest::gtest GTest::gtest_main)
target_compile_definitions(fraternite_hd_dll PRIVATE
    _CRT_SECURE_NO_WARNINGS
    PROXY_DLL_PATH="$<TARGET_FILE:proxy>"
    DRIVER_EXE_PATH="$<TARGET_FILE:fraternite_hd_driver>"
    LOG_FILE_PATH="proxy_log.txt"
    # Parens break MSBuild /D escaping, so match the prefix only.
    EXPECTED_BANNER="=== FRATERNITE_HD "
)
add_dependencies(fraternite_hd_dll proxy fraternite_hd_driver)
gtest_discover_tests(fraternite_hd_dll PROPERTIES LABELS "SYSTEM")
