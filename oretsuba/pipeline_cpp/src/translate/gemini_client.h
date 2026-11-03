// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <boost/json.hpp>

#include "translate/glossary.h"

namespace oretsuba::translate {

constexpr int kDefaultTimeoutSec = 600;
constexpr std::size_t kFallbackSubBatchSize = 20U;

struct RateLimitError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct ApiError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Strategy interface for batch translation backends.
class ITranslationBackend {
public:
    virtual ~ITranslationBackend();

    virtual std::map<int, std::string> TranslateBatch(
        const std::vector<std::string>& prompt_lines,
        const std::vector<std::string>& previous_context,
        const std::vector<std::string>& context_after,
        bool use_neutral_prompt = false) = 0;
};

// Thread-compatible HTTPS client for gemini-4-argon over Boost.Beast + OpenSSL.
class GeminiClient final : public ITranslationBackend {
public:
    explicit GeminiClient(std::string api_key = {},
                          std::string model = kDefaultModel,
                          std::string project_dir = {});
    GeminiClient(const GeminiClient&) = delete;
    GeminiClient& operator=(const GeminiClient&) = delete;
    GeminiClient(GeminiClient&& other) noexcept;
    GeminiClient& operator=(GeminiClient&& other) noexcept;
    ~GeminiClient() override;

    std::map<int, std::string> TranslateBatch(
        const std::vector<std::string>& prompt_lines,
        const std::vector<std::string>& previous_context,
        const std::vector<std::string>& context_after,
        bool use_neutral_prompt = false) override;

    boost::json::value Messages(const boost::json::value& body,
                                int timeout_sec = kDefaultTimeoutSec);

    const std::string& Model() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::string LoadGeminiApiKey(const std::string& project_dir = {});

std::string SanitizeAscii(std::string_view text);
std::string StripSpeakerTags(std::string_view text);

std::string BuildUserPrompt(const std::vector<std::string>& batch_lines,
                            const std::vector<std::string>& previous_context,
                            const std::vector<std::string>& context_after = {});

std::map<int, std::string> ParseNumberedResponse(std::string_view response_text);

std::map<int, std::string> TranslateBatchWithFallback(
    ITranslationBackend& backend,
    const std::vector<std::string>& prompt_lines,
    const std::vector<std::string>& previous_context,
    const std::vector<std::string>& context_after = {});

}  // namespace oretsuba::translate
