// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Gemini API client interface (POST /v1beta/models/...:generateContent).
//
// LazyClient defers API key lookup and client construction until the first
// uncached batch actually needs an API call, so a fully-cached run succeeds
// without GEMINI_API_KEY set.
#pragma once

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

#include <boost/json.hpp>

namespace exm::gemini {

class RateLimitError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ApiError : public std::runtime_error {
public:
    explicit ApiError(int status, const std::string& body)
        : std::runtime_error("HTTP " + std::to_string(status) + ": " + body),
          status_(status) {}

    int status() const noexcept { return status_; }

private:
    int status_;
};

class Client {
public:
    virtual ~Client();
    virtual boost::json::value generate_content(const std::string& model,
                                                const boost::json::value& body) = 0;

protected:
    Client() = default;
    Client(const Client&) = default;
    Client& operator=(const Client&) = default;
    Client(Client&&) = default;
    Client& operator=(Client&&) = default;
};

std::unique_ptr<Client> make_https_client(std::string api_key = {});

void load_api_key(const std::string& project_dir);

class LazyClient {
public:
    using Factory = std::function<std::unique_ptr<Client>()>;

    explicit LazyClient(Factory factory = [] { return make_https_client(); });
    ~LazyClient();

    LazyClient(const LazyClient&) = delete;
    LazyClient& operator=(const LazyClient&) = delete;
    LazyClient(LazyClient&&) noexcept;
    LazyClient& operator=(LazyClient&&) noexcept;

    Client& get();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace exm::gemini
