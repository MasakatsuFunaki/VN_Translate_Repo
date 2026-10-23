# Test pyramid for sakuranotoki proxy_dll.
#
# Three tiers, each its own subdirectory:
#   UT/      — unit tests (one file per class/module: foo_ut.cpp)
#   CT/      — component tests (one file per component: foo_ct.cpp)
#   SYSTEM/  — black-box tests of the built DLL (sakuranotoki_dll.cpp)
#
# Subdirs are conditional so games without a *_logic.cpp module can still
# enable tests (only SYSTEM tier is generated for those — UT/CT come back
# as soon as a logic library exists).
#
# Run only one tier:
#   ctest -L UT      (in build dir)
#   ctest -L CT
#   ctest -L SYSTEM

# UT and CT reference $<TARGET_OBJECTS:translator_logic>, which is the
# OBJECT-library version of translator_logic.cpp. In sakuranotoki's
# parent CMakeLists.txt the file is compiled directly into version_proxy
# (no separate OBJECT lib), so the target doesn't exist and adding these
# subdirs is a configure-time error. Guard until the parent CMakeLists
# carves out an OBJECT lib for translator_logic.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/UT/CMakeLists.txt" AND TARGET translator_logic)
    add_subdirectory(UT)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/CT/CMakeLists.txt" AND TARGET translator_logic)
    add_subdirectory(CT)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/SYSTEM/CMakeLists.txt")
    add_subdirectory(SYSTEM)
endif()
