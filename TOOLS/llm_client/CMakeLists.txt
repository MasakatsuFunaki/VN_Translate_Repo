cmake_minimum_required(VERSION 3.23)
project(VnLlmClient LANGUAGES CXX)

set(VN_LLM_CLIENT_IS_STANDALONE OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(VN_LLM_CLIENT_IS_STANDALONE ON)
endif()

if(NOT TARGET Boost::json OR NOT TARGET Boost::headers)
    find_package(Boost REQUIRED COMPONENTS json)
endif()
if(NOT TARGET openssl::openssl)
    find_package(OpenSSL REQUIRED)
endif()

function(vn_llm_configure_library target_name)
    set_target_properties(${target_name} PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    target_compile_features(${target_name}
        PUBLIC cxx_std_17
        PRIVATE cxx_std_20
    )
    target_include_directories(${target_name}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    if(MSVC)
        target_compile_options(${target_name} PRIVATE
            /W4
            /utf-8
            /bigobj
            /external:anglebrackets
            /external:W0
        )
    endif()
endfunction()

# Level 1: Pure C++20 domain value types, protocol anchors, retry policy
# templates, and zero-thought circuit breaker. Zero Boost/OpenSSL/Win32 deps.
add_library(vn_llm_core STATIC
    include/vn/llm/llm_client.h
    include/vn/llm/llm_types.h
    include/vn/llm/zero_thought_guard.h
    src/core/ascii_text.h
    src/core/llm_port.cpp
    src/core/llm_types.cpp
    src/core/retry_executor.h
    src/core/zero_thought_guard.cpp
)
add_library(vn::llm_core ALIAS vn_llm_core)
vn_llm_configure_library(vn_llm_core)

# Level 2: Gemini v1beta wire codec, PIMPL Boost.Beast/OpenSSL/WinCrypt TLS
# transport, credential resolution, and PIMPL GeminiClientAdapter.
# Isolates all third-party and OS libraries strictly PRIVATE.
add_library(vn_llm_gemini STATIC
    include/vn/llm/gemini_wire_codec.h
    src/adapters/gemini/gemini_client_adapter.cpp
    src/adapters/gemini/gemini_client_adapter.h
    src/adapters/gemini/gemini_wire_codec.cpp
    src/transport/beast_tls_transport.cpp
    src/transport/beast_tls_transport.h
    src/transport/env_credentials.cpp
    src/transport/env_credentials.h
)
add_library(vn::llm_gemini ALIAS vn_llm_gemini)
vn_llm_configure_library(vn_llm_gemini)
target_compile_definitions(vn_llm_gemini PRIVATE
    _CRT_SECURE_NO_WARNINGS
    _WIN32_WINNT=0x0601
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_link_libraries(vn_llm_gemini
    PUBLIC
        vn_llm_core
    PRIVATE
        Boost::json
        Boost::headers
        openssl::openssl
        ws2_32
        crypt32
        advapi32
)

# Level 3: Top-level parameterized factory (`create_llm_client`) escalating
# concrete provider selection above the protocol and adapter packages.
add_library(vn_llm_client STATIC
    src/llm_factory.cpp
)
add_library(vn::llm_client ALIAS vn_llm_client)
vn_llm_configure_library(vn_llm_client)
target_link_libraries(vn_llm_client
    PUBLIC
        vn_llm_core
        vn_llm_gemini
)

if(VN_LLM_CLIENT_IS_STANDALONE)
    option(ENABLE_TESTS "Build the vn::llm_client GoogleTest suite" ON)
    if(ENABLE_TESTS)
        find_package(GTest REQUIRED)
        enable_testing()
        include(GoogleTest)
        add_subdirectory(tests)
    endif()
endif()
