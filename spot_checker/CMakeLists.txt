cmake_minimum_required(VERSION 3.23)
project(SpotChecker LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /utf-8 /bigobj /external:anglebrackets /external:W0)
endif()

option(ENABLE_TESTS "Build the GoogleTest suite" ON)

find_package(Boost REQUIRED COMPONENTS json program_options)
find_package(OpenSSL REQUIRED)

# --------------------------------------------------------------------
# L1..L3: Foundation, domain value types, boundary ports, and JSON/script storage.
# Zero OpenSSL, WinSock, WinCrypt, or Boost.Beast dependencies.
# --------------------------------------------------------------------
add_library(spot_checker_foundation STATIC
    src/common/status.cpp
    src/common/status.h
    src/common/utf8_text.cpp
    src/common/utf8_text.h
    src/common/win32_cli_env.cpp
    src/common/win32_cli_env.h
    src/domain/spot_check_types.cpp
    src/domain/spot_check_types.h
    src/domain/vn_line_syntax.cpp
    src/domain/vn_line_syntax.h
    src/domain/tone_glossary.cpp
    src/domain/tone_glossary.h
    src/ports/llm_transport.cpp
    src/ports/llm_transport.h
    src/ports/file_store.cpp
    src/ports/file_store.h
    src/ports/crypt_sync.cpp
    src/ports/crypt_sync.h
    src/ports/clock_and_logger.cpp
    src/ports/clock_and_logger.h
    src/storage/json_format_style.cpp
    src/storage/json_format_style.h
    src/storage/script_repository.cpp
    src/storage/script_repository.h
)
target_include_directories(spot_checker_foundation PUBLIC src)
target_compile_definitions(spot_checker_foundation PUBLIC
    _CRT_SECURE_NO_WARNINGS
    _WIN32_WINNT=0x0601
)
target_link_libraries(spot_checker_foundation PRIVATE
    Boost::json
    shell32
)

# --------------------------------------------------------------------
# L3..L4: Gemini 4 Argon wire codec and HTTPS/TLS transport client.
# Isolates Boost.Beast, OpenSSL, WinSock, and WinCrypt privately.
# --------------------------------------------------------------------
add_library(spot_checker_argon_client STATIC
    src/client/argon_wire_codec.cpp
    src/client/argon_wire_codec.h
    src/client/argon_client.cpp
    src/client/argon_client.h
)
target_link_libraries(spot_checker_argon_client PUBLIC
    spot_checker_foundation
)
target_link_libraries(spot_checker_argon_client PRIVATE
    Boost::json
    Boost::headers
    openssl::openssl
    ws2_32
    crypt32
)

# --------------------------------------------------------------------
# L3..L5: Statistical hotspot detector, window planner, prompt codec,
# deterministic post-edit validator, checkpoint store, and SpotChecker facade.
# --------------------------------------------------------------------
add_library(spot_checker_engine STATIC
    src/detector/weakness_signals.cpp
    src/detector/weakness_signals.h
    src/detector/hotspot_detector.cpp
    src/detector/hotspot_detector.h
    src/checker/window_planner.cpp
    src/checker/window_planner.h
    src/checker/prompt_codec.cpp
    src/checker/prompt_codec.h
    src/checker/fix_validator.cpp
    src/checker/fix_validator.h
    src/checker/spot_check_checkpoint.cpp
    src/checker/spot_check_checkpoint.h
    src/checker/spot_checker.cpp
    src/checker/spot_checker.h
)
target_link_libraries(spot_checker_engine PUBLIC
    spot_checker_foundation
)
target_link_libraries(spot_checker_engine PRIVATE
    spot_checker_argon_client
    Boost::json
)

# --------------------------------------------------------------------
# L6: Packaged CLI executable (`01_spot_check.exe`).
# --------------------------------------------------------------------
add_executable(01_spot_check
    src/apps/spot_check_main.cpp
)
target_link_libraries(01_spot_check PRIVATE
    spot_checker_engine
    Boost::program_options
)

install(TARGETS 01_spot_check
        RUNTIME DESTINATION bin)

if(ENABLE_TESTS)
    find_package(GTest REQUIRED)
    enable_testing()
    include(GoogleTest)

    # Tier 1: Unit Tests (UT)
    add_subdirectory(tests/UT/utf8_text)
    add_subdirectory(tests/UT/vn_line_syntax)
    add_subdirectory(tests/UT/win32_cli_env)
    add_subdirectory(tests/UT/tone_glossary)
    add_subdirectory(tests/UT/json_format_style)
    add_subdirectory(tests/UT/script_repository)
    add_subdirectory(tests/UT/argon_wire_codec)
    add_subdirectory(tests/UT/weakness_signals)
    add_subdirectory(tests/UT/hotspot_detector)
    add_subdirectory(tests/UT/window_planner)
    add_subdirectory(tests/UT/prompt_codec)
    add_subdirectory(tests/UT/spot_check_checkpoint)
    add_subdirectory(tests/UT/fix_validator)

    # Tier 2: Component Tests (CT)
    add_subdirectory(tests/COMPONENT/spot_checker)

    # Tier 3: System Tests (ST)
    add_subdirectory(tests/SYSTEM/cli_offline)
    add_subdirectory(tests/SYSTEM/h_scene_detection)
    add_subdirectory(tests/SYSTEM/all_games_scan)
    add_subdirectory(tests/SYSTEM/live_argon_api)
endif()
