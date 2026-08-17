# Test pyramid: UT / CT / SYSTEM.  Subdirs are conditional.

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/UT/CMakeLists.txt")
    add_subdirectory(UT)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/CT/CMakeLists.txt")
    add_subdirectory(CT)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/SYSTEM/CMakeLists.txt")
    add_subdirectory(SYSTEM)
endif()
