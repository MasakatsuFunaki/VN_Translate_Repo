# Test pyramid for EXTRAVAGANZA_CE proxy_dll.
#
# Three tiers, each its own subdirectory:
#   UT/      — unit tests (one file per class/module: foo_ut.cpp)
#   CT/      — component tests (one file per component: foo_ct.cpp)
#   SYSTEM/  — black-box tests of the built DLL (extravaganza_ce_dll.cpp)
#
# Subdirs are conditional so games without a *_logic.cpp module can still
# enable tests (only SYSTEM tier is generated for those — UT/CT come back
# as soon as a logic library exists).
#
# Run only one tier:
#   ctest -L UT      (in build dir)
#   ctest -L CT
#   ctest -L SYSTEM

include(GoogleTest)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/patcher_logic_ut.cpp")
    add_executable(patcher_logic_ut
        patcher_logic_ut.cpp
        $<TARGET_OBJECTS:patcher_logic>
    )
    target_link_libraries(patcher_logic_ut PRIVATE GTest::gtest GTest::gtest_main)
    target_include_directories(patcher_logic_ut PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(patcher_logic_ut PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
        _CRT_SECURE_NO_WARNINGS
    )
    gtest_discover_tests(patcher_logic_ut PROPERTIES LABELS "UT")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/UT/CMakeLists.txt")
    add_subdirectory(UT)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/CT/CMakeLists.txt")
    add_subdirectory(CT)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/SYSTEM/CMakeLists.txt")
    add_subdirectory(SYSTEM)
endif()
