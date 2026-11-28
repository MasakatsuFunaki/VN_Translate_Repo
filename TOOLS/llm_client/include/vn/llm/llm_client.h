// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// GoF Strategy / Lakos Protocol interface (`ILlmClient`), raw HTTP seam
// (`IRawHttpTransport`), parameterized factory (`create_llm_client`), and
// side-effect-free credential helpers.
// Standard STL only -- zero Boost, OpenSSL, or OS headers.
#pragma once

#include "vn/llm/llm_types.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#if __cplusplus >= 202002L || (defined(_MSVC_LANG) && _MSVC_LANG >= 202002L)
#include <concepts>
#endif

namespace vn::llm {

struct RawHttpResponse {
    int status_code = kHttpStatusUnset;
    std::string body;
};

#if __cplusplus >= 202002L || (defined(_MSVC_LANG) && _MSVC_LANG >= 202002L)
template <typename T>
concept RawHttpTransportLike =
    requires(T& transport,
             std::string_view target_path,
             std::string_view json_body,
             int connect_timeout_sec,
             int read_timeout_sec) {
        {
            transport.post_json(target_path, json_body,
                                connect_timeout_sec, read_timeout_sec)
        } -> std::same_as<RawHttpResponse>;
    };

template <typename F>
concept BackoffSleepCallable =
    std::invocable<F&, std::chrono::milliseconds>;

template <typename F>
concept DiagnosticLogCallable =
    std::invocable<F&, std::string_view>;
#endif

// Optional outer HTTP transport seam for deterministic offline Component Tests.
// Thread-compatible: concurrent const access is safe; post_json() requires
// external synchronization unless documented otherwise by the implementation.
class IRawHttpTransport {
public:
    virtual ~IRawHttpTransport();

    IRawHttpTransport(const IRawHttpTransport&) = delete;
    IRawHttpTransport& operator=(const IRawHttpTransport&) = delete;

    [[nodiscard]] virtual RawHttpResponse post_json(
        std::string_view target_path,
        std::string_view json_body,
        int connect_timeout_sec,
        int read_timeout_sec) = 0;

protected:
    IRawHttpTransport() = default;
    IRawHttpTransport(IRawHttpTransport&&) noexcept = default;
    IRawHttpTransport& operator=(IRawHttpTransport&&) noexcept = default;
};

// Pure Strategy / Protocol interface for synchronous LLM invocations.
// Thread-compatible: concurrent const access is safe; generate() requires
// external synchronization.
class ILlmClient {
public:
    virtual ~ILlmClient();

    ILlmClient(const ILlmClient&) = delete;
    ILlmClient& operator=(const ILlmClient&) = delete;

    [[nodiscard]] virtual LlmResponse generate(const LlmRequest& request) = 0;

protected:
    ILlmClient() = default;
    ILlmClient(ILlmClient&&) noexcept = default;
    ILlmClient& operator=(ILlmClient&&) noexcept = default;
};

struct LlmClientOptions {
    LlmProvider provider = LlmProvider::Gemini;
    std::string api_key;
    std::string default_model{kDefaultModel};
    int max_retries = kDefaultMaxRetries;
    // Max retries when an unblocked guarded Gemini 4 call returns 0 thought
    // tokens (`kAutoZeroThoughtRetries = -1` -> `kDefaultZeroThoughtRetries` on
    // live HTTPS transport, `0` on injected `raw_transport` unless set >= 0).
    int zero_thought_max_retries = kAutoZeroThoughtRetries;
    int connect_timeout_seconds = kDefaultConnectTimeoutSeconds;
    int read_timeout_seconds = kDefaultReadTimeoutSeconds;
    std::chrono::milliseconds initial_backoff = kDefaultInitialBackoff;
    std::function<void(std::chrono::milliseconds)> sleep_fn;
    std::function<void(std::string_view)> log_fn;
    IRawHttpTransport* raw_transport = nullptr;
};

// Parameterized factory returning a PIMPL-insulated Object Adapter (`GeminiClientAdapter`
// for `LlmProvider::Gemini`) backed by Windows `ROOT` cert store + Boost.Beast/OpenSSL
// TLS 1.2+ (or `options.raw_transport` when non-null).
// Throws `LlmError` immediately if `options.default_model` is disallowed
// (`is_disallowed_model`, e.g. any `"argon"` model identifier).
[[nodiscard]] std::unique_ptr<ILlmClient> create_llm_client(
    LlmClientOptions options = {});

// Resolves `GEMINI_API_KEY` from process environment (`_wgetenv`), then
// `%USERPROFILE%\.gemini\.env` (or `dotenv_override` exclusively when non-empty),
// then `HKCU\Environment` without mutating process environment or logging keys.
[[nodiscard]] std::string resolve_gemini_api_key(
    const std::filesystem::path& dotenv_override = {});

// Pure, side-effect-free `.env` parser supporting `KEY=VAL`, `export KEY=VAL`,
// `#` full-line and unquoted inline comments, UTF-8 BOM, and single/double
// quotes with escape sequences. Last matching assignment wins.
[[nodiscard]] std::string parse_dotenv_value(std::string_view dotenv_contents,
                                             std::string_view key_name);

}  // namespace vn::llm
