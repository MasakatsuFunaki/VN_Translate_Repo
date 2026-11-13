cmake_minimum_required(VERSION 3.23)
project(KishinHishoTools LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# /utf-8: the analysis reports contain an em dash and arrows, and both are
# diffed byte-for-byte against committed golden logs in st_alpharom_samples.
# /W4: item 1 of the house C++ standard -- compile cleanly at high warning levels.
if(MSVC)
    add_compile_options(/utf-8 /W4)
endif()

option(ENABLE_TESTS "Build the GoogleTest suite" ON)

find_package(Boost REQUIRED COMPONENTS json regex program_options)
find_package(capstone REQUIRED)
find_package(ZLIB REQUIRED)

# Only the SYSTEM sample suite gets an anchor to the checkout fixtures and
# golden logs.  Unit tests (ut_*) and installed apps must NOT get one: unit
# tests stay synthetic and apps find the project at runtime (src/apps/paths.cpp).
get_filename_component(KSH_PROJECT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# ksh_core: everything except the app mains, shared by apps and tests.
add_library(ksh_core STATIC
    src/common/util.cpp
    src/common/util.h
    src/pe/pe_image.cpp
    src/pe/pe_image.h
    src/pe/disasm.cpp
    src/pe/disasm.h
    src/minidump/minidump.cpp
    src/minidump/minidump.h
    src/patch/deploy.cpp
    src/patch/deploy.h
    src/pak/pak_archive.cpp
    src/pak/pak_archive.h
    src/pak/nps_rebuilder.cpp
    src/pak/nps_rebuilder.h
    src/analysis/recon.cpp
    src/analysis/dumpreport.cpp
    src/analysis/reports.h
)
target_include_directories(ksh_core PUBLIC src)
target_compile_definitions(ksh_core PUBLIC
    _CRT_SECURE_NO_WARNINGS
    _WIN32_WINNT=0x0601
)
target_link_libraries(ksh_core PUBLIC
    Boost::json
    Boost::regex
    capstone::capstone
    ZLIB::ZLIB
)

# Tools: the deploy driver and the .pak script rebuilder.
function(ksh_app target main)
    add_executable(${target} ${main} src/apps/paths.h src/apps/paths.cpp)
    target_link_libraries(${target} PRIVATE ksh_core Boost::program_options shell32)
endfunction()

ksh_app(01_patch     src/apps/patch_main.cpp)
ksh_app(02_build_pak src/apps/build_pak_main.cpp)

# Install: bin/ is what the user runs.
#
# The root project collects both halves into build/install, and the two
# halves must not share a folder -- helper_dll is 32-bit and carries a DLL
# the loader would happily pick up next to these 64-bit executables. So the
# Win32 artifacts install into game/ and these into bin/.
#
# ksh_core is an internal static library and the test binaries are not
# user-facing, so neither is installed.
install(TARGETS
            01_patch
            02_build_pak
        RUNTIME DESTINATION bin)

if(ENABLE_TESTS)
    find_package(GTest REQUIRED)
    enable_testing()
    add_subdirectory(tests)
endif()
