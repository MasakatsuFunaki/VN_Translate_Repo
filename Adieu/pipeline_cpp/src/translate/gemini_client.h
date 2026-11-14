// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Google Gemini generateContent HTTPS client interface and PIMPL-insulated adapters.
#pragma once

#include <memory>
#include <stdexcept>
#include <string>

#include <boost/json.hpp>

namespace adi::gemini {

inline constexpr int kDefaultRequestTimeoutSec = 600;

// HTTP 429 -- distinct from ApiError so the caller can back off and retry.
struct RateLimitError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Any other non-2xx HTTP status, TLS verification failure, or transport error.
struct ApiError : std::runtime_error {
    using std::runtime_error::runtime_error;
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

// Env-or-dotenv key loader: honours GEMINI_API_KEY, else reads GEMINI_API_KEY
// from ~/.gemini/.env and exports it into the current process environment.
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
