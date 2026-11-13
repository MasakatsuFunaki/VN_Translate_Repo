# Test pyramid for Mushigurui proxy_dll.

include(GoogleTest)

add_executable(ut_patcher_logic
    ut_patcher_logic.cpp
    $<TARGET_OBJECTS:translator_logic>
)
target_link_libraries(ut_patcher_logic PRIVATE
    GTest::gtest
    GTest::gtest_main
)
target_include_directories(ut_patcher_logic PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_definitions(ut_patcher_logic PRIVATE
    _CRT_SECURE_NO_WARNINGS
)
gtest_discover_tests(ut_patcher_logic PROPERTIES LABELS "UT")

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/UT/CMakeLists.txt")
    add_subdirectory(UT)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/CT/CMakeLists.txt")
    add_subdirectory(CT)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/SYSTEM/CMakeLists.txt")
    add_subdirectory(SYSTEM)
endif()
