cmake_minimum_required(VERSION 3.23)
project(OretsubaProxy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "winmm.dll must be built as 32-bit (x86) because ORE_TUBA.exe is a 32-bit process! "
        "Current pointer size: ${CMAKE_SIZEOF_VOID_P} bytes. "
        "Re-configure with preset 'conan-windows-x86-release' or pass -A Win32 to cmake."
    )
endif()

if(MSVC)
    add_compile_options(/W4 /utf-8 /external:anglebrackets /external:W0)
endif()

option(ENABLE_TESTS "Build the test suites (UT, CT, SYSTEM)" ON)

add_library(translator_logic STATIC
    src/translator_logic.cpp
    src/translator_logic.h
    src/runtime_hooks.cpp
    src/runtime_hooks.h
)

target_include_directories(translator_logic PUBLIC src)
target_compile_definitions(translator_logic PUBLIC _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(translator_logic PUBLIC gdi32 user32)

add_library(proxy SHARED
    src/dllmain.cpp
    src/proxy.cpp
    src/proxy.h
    src/log.h
    src/winmm.def
)

target_include_directories(proxy PRIVATE src)
target_compile_definitions(proxy PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(proxy PRIVATE translator_logic gdi32 user32)

set_target_properties(proxy PROPERTIES
    OUTPUT_NAME "winmm"
    PREFIX ""
)

install(TARGETS proxy RUNTIME DESTINATION game)

if(ENABLE_TESTS)
    find_package(GTest REQUIRED)
    enable_testing()
    add_subdirectory(tests)
endif()
