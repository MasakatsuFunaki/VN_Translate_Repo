# Test pyramid for CROSS_CHANNEL proxy_dll.
#
# Three tiers, each its own subdirectory:
#   UT/      — unit tests (one file per class/module: foo_ut.cpp)
#   CT/      — component tests (one file per component: foo_ct.cpp)
#   SYSTEM/  — black-box tests of the built DLL (cross_channel_dll.cpp)
#
# Subdirs are conditional so games without a *_logic.cpp module can still
# enable tests (only SYSTEM tier is generated for those — UT/CT come back
# as soon as a logic library exists).
#
# Run only one tier:
#   ctest -L UT      (in build dir)
#   ctest -L CT
#   ctest -L SYSTEM

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/UT/CMakeLists.txt")
    add_subdirectory(UT)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/CT/CMakeLists.txt")
    add_subdirectory(CT)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/SYSTEM/CMakeLists.txt")
    add_subdirectory(SYSTEM)
endif()
