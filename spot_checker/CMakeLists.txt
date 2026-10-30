cmake_minimum_required(VERSION 3.23)
project(SpotChecker LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/utf-8 /bigobj)
endif()

option(ENABLE_TESTS "Build the GoogleTest suite" ON)

find_package(Boost REQUIRED COMPONENTS json regex program_options)
find_package(OpenSSL REQUIRED)

add_library(spot_checker_core STATIC
    src/common/util.cpp
    src/common/util.h
    src/client/argon_client.cpp
    src/client/argon_client.h
    src/checker/hotspot_detector.cpp
    src/checker/hotspot_detector.h
    src/checker/spot_checker.cpp
    src/checker/spot_checker.h
)
target_include_directories(spot_checker_core PUBLIC src)
target_compile_definitions(spot_checker_core PUBLIC
    _CRT_SECURE_NO_WARNINGS
    _WIN32_WINNT=0x0601
)
target_link_libraries(spot_checker_core PUBLIC
    Boost::json
    Boost::regex
    Boost::headers
    openssl::openssl
    ws2_32
    crypt32
)

add_executable(01_spot_check
    src/apps/spot_check_main.cpp
)
target_link_libraries(01_spot_check PRIVATE
    spot_checker_core
    Boost::program_options
)

install(TARGETS 01_spot_check
        RUNTIME DESTINATION bin)

if(ENABLE_TESTS)
    find_package(GTest REQUIRED)
    enable_testing()
    add_subdirectory(tests)
endif()
