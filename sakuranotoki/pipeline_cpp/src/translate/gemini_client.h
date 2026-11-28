// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Thin adapter bridging snt::translate's Boost.JSON messages contract onto the
// shared `vn::llm_client` (`ILlmClient`, `gemini_wire_codec`, and
// `ZeroThoughtCircuitBreaker`).
#pragma once

#include "vn/llm/llm_types.h"

#include <boost/json.hpp>

#include <memory>
#include <stdexcept>
#include <string>

namespace vn::llm {
class IRawHttpTransport;
}

namespace snt::gemini {

inline constexpr int kDefaultTimeoutSec = vn::llm::kDefaultReadTimeoutSeconds;

// HTTP 429 -- distinct from ApiError because the caller waits and retries
// instead of failing the batch.
struct RateLimitError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Any other non-2xx response or transport failure.
struct ApiError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Raised when ZeroThoughtCircuitBreaker trips on guarded Gemini 4 reasoning
// (such as gemini-4-carbon MEDIUM) returning 0 thoughts.
struct ZeroThoughtError : ApiError {
    using ApiError::ApiError;
};

// Build the Gemini generateContent request body via `vn::llm::build_gemini_request_json`,
// writing the resolved model ID (`vn::llm::kDefaultModel` by default) to
// `out_model` when non-null. Throws `ApiError` if `in_obj["model"]` names a
// disallowed model (such as any `"argon"` model or embedded NUL byte).
boost::json::object build_gemini_request(const boost::json::object& in_obj,
                                         std::string* out_model = nullptr);

// Adapt a raw Gemini generateContent response JSON via
// `vn::llm::parse_gemini_response_json` into the pipeline's normalized response
// object (`content`, `stop_reason`, and `usage` including `candidates_tokens`
// and `thought_tokens`).
boost::json::value adapt_gemini_response(const boost::json::value& gem_val);

class Client {
public:
    // Key resolved via `vn::llm::resolve_gemini_api_key()` when `api_key` is
    // empty; optional `raw_transport` enables deterministic offline testing.
    explicit Client(std::string api_key = {},
                    vn::llm::IRawHttpTransport* raw_transport = nullptr);
    ~Client();

    Client(Client&& other) noexcept;
    Client& operator=(Client&& other) noexcept;

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Invoke `vn::llm::ILlmClient::generate` + `ZeroThoughtCircuitBreaker` and
    // adapt the response to the pipeline's messages JSON shape.
    boost::json::value messages(const boost::json::value& body,
                                int timeout_sec = kDefaultTimeoutSec);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Side-effect-free key resolver delegating to `vn::llm::resolve_gemini_api_key()`.
std::string load_api_key();

// Builds the Client on first use, so a run whose cache already covers every
// line makes no request and needs no key.
class LazyClient {
public:
    LazyClient();
    ~LazyClient();

    LazyClient(LazyClient&& other) noexcept;
    LazyClient& operator=(LazyClient&& other) noexcept;

    LazyClient(const LazyClient&) = delete;
    LazyClient& operator=(const LazyClient&) = delete;

    // Throws ApiError when a request is finally required and no key is set.
    Client& get();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace snt::gemini
