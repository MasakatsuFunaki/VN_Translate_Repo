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

namespace shin::anthropic {

// HTTP 429 -- caller waits and retries.
struct RateLimitError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Any other non-2xx response or transport failure.
struct ApiError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Client {
public:
    // Key from ANTHROPIC_API_KEY env var (or explicit).  Mirrors
    // anthropic.Anthropic() -- throws ApiError when no key is available.
    explicit Client(std::string api_key = {});

    // POST /v1/messages with `body` (already-built request JSON).
    // Returns the parsed response JSON.  Throws RateLimitError on 429,
    // ApiError otherwise.  timeout_sec covers connect+write+read.
    boost::json::value messages(const boost::json::value& body, int timeout_sec = 600);

private:
    std::string api_key_;
};

// Env-or-settings key loader: honours ANTHROPIC_API_KEY, else reads
// primaryApiKey from ~/.claude_vn/settings.json (stripping a leading "echo ")
// and exports it to the environment.  Returns the key or "".
std::string load_api_key();

// Builds the Client on first use, so a run whose cache already covers every
// line makes no request and needs no key.
class LazyClient {
public:
    // Throws ApiError when a request is finally required and no key is set.
    Client& get();

    // get() hands out a reference into client_, so copying would leave that
    // reference pointing into the wrong object.
    LazyClient() = default;
    LazyClient(const LazyClient&) = delete;
    LazyClient& operator=(const LazyClient&) = delete;

private:
    std::optional<Client> client_;
};

}  // namespace shin::anthropic
