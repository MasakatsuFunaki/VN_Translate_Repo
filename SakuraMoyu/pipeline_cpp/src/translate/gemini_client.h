// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Thin adapter over `vn::llm_client` for the SakuraMoyu translation pipeline.
#pragma once

#include "vn/llm/llm_types.h"

#include <boost/json.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fav::gemini {

inline constexpr std::int64_t kMinOutputTokens = vn::llm::kMinOutputTokensFloor;
inline constexpr std::int64_t kMaxOutputTokens = vn::llm::kMaxOutputTokensCeiling;
inline constexpr int kDefaultTimeoutSec = vn::llm::kDefaultReadTimeoutSeconds;
inline constexpr const char* kDefaultModel = vn::llm::kDefaultModel.data();
inline constexpr const char* kDefaultThinkingLevel = vn::llm::kDefaultThinkingLevel.data();

struct Usage {
    std::int64_t input_tokens = 0;
    std::int64_t output_tokens = 0;
    std::int64_t thought_tokens = 0;
    std::int64_t cache_read_input_tokens = 0;
    std::int64_t cache_creation_input_tokens = 0;
};

// HTTP 429 -- caller waits and retries.
struct RateLimitError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Any other non-2xx response or transport failure.
struct ApiError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Wire-level transport seam for `POST /v1beta/models/<model>:generateContent`.
class Endpoint {
public:
    virtual ~Endpoint();
    virtual boost::json::value generate_content(
        std::string_view model,
        const boost::json::object& wire_request,
        int timeout_sec) = 0;
};

// Resolves the target model name from `body["model"]`, falling back to
// `kDefaultModel` ("gemini-4-carbon") when absent or empty, and throwing
// `ApiError` when `vn::llm::ensure_supported_model` rejects a disallowed model
// (such as any `"argon"` model or embedded NUL byte).
std::string resolve_request_model(const boost::json::value& body);

// Resolves the thinking level from `body["thinking_level"]`, falling back to
// `kDefaultThinkingLevel` ("MEDIUM") when absent or empty.
std::string resolve_thinking_level(const boost::json::value& body);

// Adapts an internal messages request `body` (`system`, `messages`,
// `max_tokens`, `temperature`, optional `thinking_level`) into the Gemini
// v1beta `generateContent` wire JSON payload:
//   - `generationConfig.maxOutputTokens` clamped to `[16384, 65536]`
//     (defaulting to `16384` when `max_tokens` is omitted).
//   - `generationConfig.thinkingConfig` set to `{"thinkingLevel": <level>}`
//     (default `"MEDIUM"`, never emitting `thinkingBudget`).
//   - `safetySettings` populated with all 5 `HARM_CATEGORY_*` categories set
//     to `"BLOCK_NONE"` (`HARASSMENT`, `HATE_SPEECH`, `SEXUALLY_EXPLICIT`,
//     `DANGEROUS_CONTENT`, `CIVIC_INTEGRITY`).
boost::json::object build_generate_content_request(const boost::json::value& body);

// Parses a Gemini v1beta `generateContent` wire JSON response into the
// normalized response object:
//   - `content`: single `{"type": "text", "text": <joined>}` block joining
//     all `candidates[0].content.parts[*].text` entries EXCEPT parts where
//     `"thought": true`.
//   - `stop_reason`: `"end_turn"` (`STOP` or default), `"max_tokens"`
//     (`MAX_TOKENS`), `"refusal"` (`SAFETY`, `RECITATION`,
//     `PROHIBITED_CONTENT`, `BLOCKLIST`, `SPII`, or
//     `promptFeedback.blockReason`), or the raw `finishReason` string.
//   - `usage`: `{input_tokens, output_tokens, thought_tokens,
//     cache_read_input_tokens, cache_creation_input_tokens}` where
//     `thought_tokens = usageMetadata.thoughtsTokenCount` and
//     `output_tokens = candidatesTokenCount + thoughtsTokenCount`.
boost::json::object parse_generate_content_response(const boost::json::value& raw_response);

// Extracts typed token usage telemetry from a normalized `Client::messages`
// response JSON value (returning zeros for absent fields).
Usage extract_usage(const boost::json::value& response);

class Client {
public:
    // Key from GEMINI_API_KEY env var or ~/.gemini/.env (or explicit).  Throws
    // ApiError when no key is available.
    explicit Client(std::string api_key = {});
    // Constructs a Client backed by a custom non-null `endpoint` without
    // reading GEMINI_API_KEY.  Throws ApiError when `endpoint == nullptr`.
    explicit Client(Endpoint* endpoint);
    ~Client();

    Client(Client&& other) noexcept;
    Client& operator=(Client&& other) noexcept;

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // POST /v1beta/models/<model>:generateContent with `body` adapted from the
    // request JSON.  Returns the parsed response JSON.  Throws RateLimitError
    // on 429, ApiError otherwise.  timeout_sec covers connect+write+read.
    boost::json::value messages(const boost::json::value& body,
                                int timeout_sec = kDefaultTimeoutSec);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Env-or-dotenv key loader: honours GEMINI_API_KEY, else reads GEMINI_API_KEY
// from ~/.gemini/.env and exports it to the environment.  Returns the key or "".
std::string load_api_key();

// Builds the Client on first use, so a run whose cache already covers every
// line makes no request and needs no key.
class LazyClient {
public:
    explicit LazyClient(Endpoint* endpoint = nullptr) noexcept;
    LazyClient(const LazyClient&) = delete;
    LazyClient& operator=(const LazyClient&) = delete;

    // Throws ApiError when a request is finally required and no key is set
    // (unless constructed with a custom non-null Endpoint).
    Client& get();

private:
    Endpoint* endpoint_ = nullptr;
    std::optional<Client> client_;
};

}  // namespace fav::gemini
