// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Minimal Anthropic Messages API client over Boost.Beast + OpenSSL.
#pragma once

#include <stdexcept>
#include <string>
#include <optional>

#include <boost/json.hpp>

namespace frat::anthropic {

// Distinct from ApiError so the caller can wait-and-retry on 429.
struct RateLimitError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct ApiError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Client {
public:
    explicit Client(std::string api_key = {});
    boost::json::value messages(const boost::json::value& body, int timeout_sec = 600);

private:
    std::string api_key_;
};

// Loads key from ANTHROPIC_API_KEY or ~/.claude_vn/settings.json. Returns "".
std::string load_api_key();

// Defers Client construction until the first API call is needed.
class LazyClient {
public:
    Client& get();

    LazyClient() = default;
    LazyClient(const LazyClient&) = delete;
    LazyClient& operator=(const LazyClient&) = delete;

private:
    std::optional<Client> client_;
};

}  // namespace frat::anthropic
