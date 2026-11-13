get_filename_component(ORETSUBA_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

add_library(oretsuba_test_hook SHARED oretsuba_dll.cpp)
target_include_directories(oretsuba_test_hook PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(oretsuba_test_hook PRIVATE translator_logic gdi32 user32)
set_target_properties(oretsuba_test_hook PROPERTIES PREFIX "")

add_executable(system_driver driver_exe.cpp)
target_include_directories(system_driver PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions(system_driver PRIVATE
    ORETSUBA_ROOT_DIR="${ORETSUBA_ROOT_DIR}"
)
target_link_libraries(system_driver PRIVATE translator_logic GTest::gtest GTest::gtest_main)
add_dependencies(system_driver oretsuba_test_hook proxy)

add_test(
    NAME SYSTEM_EndToEndHook
    COMMAND system_driver
    WORKING_DIRECTORY $<TARGET_FILE_DIR:system_driver>
)
set_tests_properties(SYSTEM_EndToEndHook PROPERTIES LABELS "SYSTEM")
