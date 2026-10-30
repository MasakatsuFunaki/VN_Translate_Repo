// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// PIMPL-insulated Google Gemini (gemini-4-argon) HTTPS client over
// Boost.Beast + OpenSSL + Windows ROOT certificate store.
#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

#include <boost/json.hpp>

namespace spot_checker {

struct RateLimitError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct ApiError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct ArgonResponse {
    std::string text;
    std::string finish_reason;
    std::int64_t prompt_tokens = 0;
    std::int64_t output_tokens = 0;
    std::int64_t thought_tokens = 0;
};

// Reads GEMINI_API_KEY from environment or ~/.gemini/.env.
std::string load_gemini_api_key();

// Builds the JSON payload for Gemini generateContent with thinkingBudget: 2048
// and responseMimeType: "application/json".
boost::json::object build_argon_request_payload(const std::string& system_prompt,
                                                const std::string& user_prompt);

// Extracts the non-thought text and token metrics from a raw generateContent
// response JSON object.
ArgonResponse parse_argon_response_payload(const boost::json::value& raw);

class ArgonClient {
public:
    explicit ArgonClient(std::string api_key = {},
                         std::string model = "gemini-4-argon");
    ~ArgonClient();

    ArgonClient(ArgonClient&& other) noexcept;
    ArgonClient& operator=(ArgonClient&& other) noexcept;

    ArgonClient(const ArgonClient&) = delete;
    ArgonClient& operator=(const ArgonClient&) = delete;

    ArgonResponse generate(const std::string& system_prompt,
                           const std::string& user_prompt,
                           int timeout_sec = 600);

    const std::string& model() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace spot_checker
