# Test pyramid for shingakkou proxy_dll.
#
# Three tiers:
#   UT      — pure standalone unit tests
#   CT      — component tests
#   SYSTEM  — black-box DLL and live-archive system tests

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/UT/CMakeLists.txt")
    add_subdirectory(UT)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/CT/CMakeLists.txt")
    add_subdirectory(CT)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/SYSTEM/CMakeLists.txt")
    add_subdirectory(SYSTEM)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ut_translator_logic.cpp")
    include(GoogleTest)
    add_executable(ut_translator_logic
        ut_translator_logic.cpp
        $<TARGET_OBJECTS:translator_logic>
    )
    target_link_libraries(ut_translator_logic PRIVATE GTest::gtest GTest::gtest_main)
    target_include_directories(ut_translator_logic PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(ut_translator_logic PRIVATE _CRT_SECURE_NO_WARNINGS)
    gtest_discover_tests(ut_translator_logic PROPERTIES LABELS "UT")
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/st_bootup_scripts.cpp")
    include(GoogleTest)
    file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/../menu_patch.bin" SHIN_MENU_PATCH_BIN_PATH_FWD)
    add_executable(st_bootup_scripts
        st_bootup_scripts.cpp
        $<TARGET_OBJECTS:translator_logic>
    )
    target_link_libraries(st_bootup_scripts PRIVATE GTest::gtest GTest::gtest_main)
    target_include_directories(st_bootup_scripts PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(st_bootup_scripts PRIVATE
        _CRT_SECURE_NO_WARNINGS
        SHIN_MENU_PATCH_BIN_PATH="${SHIN_MENU_PATCH_BIN_PATH_FWD}"
    )
    gtest_discover_tests(st_bootup_scripts PROPERTIES LABELS "SYSTEM")
endif()
