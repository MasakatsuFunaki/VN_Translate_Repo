cmake_minimum_required(VERSION 3.20)
project(AdieuProxy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/utf-8)
endif()

# SiglusEngine.exe is x86. Tests build for the same arch so they link
# the exact same logic object code that ships in winmm.dll.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
        "Must build as 32-bit! Ninja takes its architecture from the compiler "
        "environment, so configure through the x86 preset -- its Conan profile "
        "writes the conanvcvars.bat that selects the 32-bit toolchain. "
        "build.py does this for you.")
endif()

option(ENABLE_TESTS "Build the Google Test suite for Adieu proxy_dll" ON)

# OS-independent UTF-16 translation table, bypass rules, and NVL word-wrap.
add_library(translator_logic OBJECT
    src/translator_logic.cpp
    src/translator_logic.h
)
target_include_directories(translator_logic PUBLIC src)
target_compile_definitions(translator_logic PRIVATE
    _CRT_SECURE_NO_WARNINGS
)

# OS-independent UTF-16 UI chrome, menu, and dialog translation dictionary.
add_library(chrome_text_logic OBJECT
    src/chrome_text_logic.cpp
    src/chrome_text_logic.h
)
target_include_directories(chrome_text_logic PUBLIC src)
target_compile_definitions(chrome_text_logic PRIVATE
    _CRT_SECURE_NO_WARNINGS
)

# OS-independent "ADIEUMEN" v1 / "ADIEUAPL" v1 wire codec and MurmurHash2.
add_library(menu_patch_format OBJECT
    src/menu_patch_format.cpp
    src/menu_patch_format.h
)
target_include_directories(menu_patch_format PUBLIC src)
target_compile_definitions(menu_patch_format PRIVATE
    _CRT_SECURE_NO_WARNINGS
)

# Thread-safe CRITICAL_SECTION logger.
add_library(proxy_log OBJECT
    src/log.cpp
    src/log.h
)
target_include_directories(proxy_log PUBLIC src)
target_compile_definitions(proxy_log PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
)

# Loader-lock-safe kernel32-only two-phase menu asset auto-patcher.
add_library(menu_autopatch OBJECT
    src/menu_autopatch.cpp
    src/menu_autopatch.h
)
target_include_directories(menu_autopatch PUBLIC src)
target_compile_definitions(menu_autopatch PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
)

# Loader-lock-safe USER32 IAT hooks and WM_INITDIALOG translator.
add_library(win32_chrome OBJECT
    src/win32_chrome.cpp
    src/win32_chrome.h
)
target_include_directories(win32_chrome PUBLIC src)
target_compile_definitions(win32_chrome PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
)

# winmm.dll proxy dropped next to SiglusEngine.exe.
add_library(proxy SHARED
    src/dllmain.cpp
    src/proxy.cpp
    src/proxy.h
    src/translator.cpp
    src/translator.h
    src/key_dumper.cpp
    src/key_dumper.h
    src/winmm.def
    $<TARGET_OBJECTS:translator_logic>
    $<TARGET_OBJECTS:chrome_text_logic>
    $<TARGET_OBJECTS:menu_patch_format>
    $<TARGET_OBJECTS:proxy_log>
    $<TARGET_OBJECTS:menu_autopatch>
    $<TARGET_OBJECTS:win32_chrome>
)

target_compile_definitions(proxy PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
)

target_link_libraries(proxy PRIVATE
    user32
    gdi32
)

set_target_properties(proxy PROPERTIES
    OUTPUT_NAME "winmm"
    PREFIX ""
    SUFFIX ".dll"
)

target_include_directories(proxy PRIVATE src)

# --------------------------------------------------------------------
# Install: the DLL the user copies into the game, and nothing else.
#
# game/ is a separate folder from the pipeline's bin/ on purpose.  This DLL
# carries the name of a system DLL and is 32-bit, so a copy sitting beside the
# x64 executables is something the loader would pick up and then refuse.
# RUNTIME alone also excludes the import library, which nobody links against.
# --------------------------------------------------------------------
install(TARGETS proxy RUNTIME DESTINATION game)

if(ENABLE_TESTS)
    find_package(GTest CONFIG REQUIRED)
    enable_testing()
    add_subdirectory(tests)
endif()
