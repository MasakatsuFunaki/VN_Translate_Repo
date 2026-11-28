// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Thin Adieu pipeline adapter over shared `vn::llm_client` (`ILlmClient`,
// `gemini_wire_codec`, `resolve_gemini_api_key`, and `ZeroThoughtCircuitBreaker`).
#pragma once

#include "vn/llm/llm_types.h"

#include <boost/json.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace adi::gemini {

inline constexpr int kDefaultRequestTimeoutSec = vn::llm::kDefaultReadTimeoutSeconds;

// Translates an internal message-envelope request into a Gemini `generateContent`
// `(model_id, payload)` pair:
// - Rejects any disallowed `"model"` string (such as `"argon"` models or embedded
//   NUL bytes per `vn::llm::ensure_supported_model`) by throwing `ApiError`.
// - `model_id` defaults to `"gemini-4-carbon"`; overridden only when `body` is an
//   object whose `"model"` string begins with `"gemini-"`.
// - `payload["generationConfig"]["thinkingConfig"]` is `{"thinkingLevel": "MEDIUM"}`
//   (never `"thinkingBudget"`).
// - `payload["generationConfig"]["maxOutputTokens"]` defaults to `16384` and clamps
//   `body["max_tokens"]` into `[16384, 65536]` (`<= 0` or `< 16384` clamps up to
//   `16384` so `MEDIUM`/`HIGH` thinking tokens never starve candidate text;
//   `> 65536` clamps to `65536`).
// - `payload["generationConfig"]["temperature"]` is copied when `body["temperature"]`
//   is numeric (`double`, `int64`, or `uint64`), omitted otherwise.
// - `payload["safetySettings"]` contains all 5 Gemini harm categories set to
//   `"threshold": "BLOCK_NONE"`:
//     `HARM_CATEGORY_HARASSMENT`, `HARM_CATEGORY_HATE_SPEECH`,
//     `HARM_CATEGORY_SEXUALLY_EXPLICIT`, `HARM_CATEGORY_DANGEROUS_CONTENT`,
//     `HARM_CATEGORY_CIVIC_INTEGRITY`.
// - `payload["systemInstruction"]` is `{"parts": [{"text": ...}]}` when
//   `body["system"]` is a non-empty string or an array of `{"text": ...}` objects
//   (joined with `"\n"`), omitted when empty/absent.
// - `payload["contents"]` maps `body["messages"]` (`"assistant"` -> `"model"`, else
//   `"user"`; string content or `[{"type":"text","text":...},
//   {"type":"image","source":{"media_type":...,"data":...}}]` -> `inlineData`).
//   Non-object `body` produces empty `contents` alongside default `generationConfig`
//   and `safetySettings`.
std::pair<std::string, boost::json::object> build_gemini_request(
    const boost::json::value& body);

// Adapts a raw Gemini `generateContent` JSON response into the internal message
// envelope:
// - `content`: `[{"type": "text", "text": <concatenated non-thought parts>}]` from
//   `candidates[0].content.parts`, skipping any part with `"thought": true`.
// - `stop_reason`: maps `candidates[0].finishReason` (`"STOP"` or absent ->
//   `"end_turn"`, `"MAX_TOKENS"` -> `"max_tokens"`, `"SAFETY"` / `"RECITATION"` /
//   `"PROHIBITED_CONTENT"` / `"BLOCKLIST"` / `"SPII"` -> `"refusal"`, other strings
//   preserved verbatim); when `candidates` is missing/empty and
//   `promptFeedback.blockReason` is present, returns `"refusal"`.
// - `usage`: always populated with `int64` fields from `usageMetadata` (default `0`):
//   - `"input_tokens"` = `promptTokenCount`
//   - `"output_tokens"` = `candidatesTokenCount + thoughtsTokenCount`
//   - `"thought_tokens"` = `thoughtsTokenCount`
//   - `"cache_read_input_tokens"` = `cachedContentTokenCount`
//   - `"cache_creation_input_tokens"` = `0`
boost::json::value adapt_gemini_response(const boost::json::value& raw);

// HTTP 429 -- distinct from ApiError so the caller can back off and retry.
struct RateLimitError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Any other non-2xx HTTP status, TLS verification failure, or transport error.
struct ApiError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Raised when `vn::llm::ZeroThoughtCircuitBreaker` detects silent non-thinking
// degradation (`thought_tokens == 0`) on a qualifying `gemini-4-carbon` `MEDIUM` batch.
struct ZeroThoughtError : ApiError {
    using ApiError::ApiError;
};

// Pure protocol interface for batched LLM message requests. Allows unit tests
// to inject deterministic in-memory responses with zero network sockets.
class IMessageClient {
public:
    IMessageClient() = default;
    IMessageClient(const IMessageClient&) = delete;
    IMessageClient& operator=(const IMessageClient&) = delete;
    IMessageClient(IMessageClient&&) = delete;
    IMessageClient& operator=(IMessageClient&&) = delete;
    virtual ~IMessageClient();

    virtual boost::json::value messages(const boost::json::value& body,
                                        int timeout_sec = kDefaultRequestTimeoutSec) = 0;
};

// PIMPL-insulated HTTPS Gemini client. Hides OpenSSL, Boost.Beast/Asio, and
// Windows root-certificate store details from all translation units.
class Client : public IMessageClient {
public:
    explicit Client(std::string api_key = {});
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&& other) noexcept;
    Client& operator=(Client&& other) noexcept;
    ~Client() override;

    boost::json::value messages(const boost::json::value& body,
                                int timeout_sec = kDefaultRequestTimeoutSec) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Resolves `GEMINI_API_KEY` via `vn::llm::resolve_gemini_api_key()` (process
// environment, `%USERPROFILE%\.gemini\.env`, then `HKCU\Environment`).
std::string load_api_key();

// PIMPL-insulated lazy client adapter: defers credential resolution and Client
// construction until the first uncached API request is actually sent.
class LazyClient : public IMessageClient {
public:
    LazyClient();
    LazyClient(const LazyClient&) = delete;
    LazyClient& operator=(const LazyClient&) = delete;
    LazyClient(LazyClient&& other) noexcept;
    LazyClient& operator=(LazyClient&& other) noexcept;
    ~LazyClient() override;

    Client& get();

    boost::json::value messages(const boost::json::value& body,
                                int timeout_sec = kDefaultRequestTimeoutSec) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace adi::gemini
