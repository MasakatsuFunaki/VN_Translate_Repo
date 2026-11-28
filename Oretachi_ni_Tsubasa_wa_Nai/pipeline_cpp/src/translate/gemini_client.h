// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <boost/json.hpp>

#include "translate/glossary.h"
#include "vn/llm/llm_types.h"

namespace oretsuba::translate {

inline constexpr int kDefaultTimeoutSec = vn::llm::kDefaultReadTimeoutSeconds;
inline constexpr std::size_t kFallbackSubBatchSize = 20U;
inline constexpr const char* kDefaultThinkingLevel = vn::llm::kDefaultThinkingLevel.data();
inline constexpr std::int64_t kDefaultMaxOutputTokens = vn::llm::kMaxOutputTokensCeiling;

struct RateLimitError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct ApiError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct ZeroThoughtError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct GeminiUsage {
    std::int64_t input_tokens = 0;
    std::int64_t candidates_tokens = 0;
    std::int64_t thoughts_tokens = 0;
    std::int64_t output_tokens = 0;
    std::int64_t cache_read_input_tokens = 0;
    std::int64_t total_tokens = 0;
};

struct HttpResponse {
    unsigned status_code = 0;
    std::string body;
};

using HttpTransportFn = std::function<HttpResponse(
    std::string_view target,
    std::string_view request_body,
    int timeout_sec)>;

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

// Thread-compatible HTTPS client for gemini-4-carbon over Boost.Beast + OpenSSL.
class GeminiClient final : public ITranslationBackend {
public:
    explicit GeminiClient(std::string api_key = {},
                          std::string model = kDefaultModel,
                          std::string project_dir = {});
    GeminiClient(std::string api_key,
                 std::string model,
                 std::string project_dir,
                 HttpTransportFn transport_override);
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
    GeminiUsage LastUsage() const noexcept;
    GeminiUsage CumulativeUsage() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

boost::json::object BuildGenerateContentRequest(const boost::json::value& body);
boost::json::object ParseGenerateContentResponse(
    const boost::json::value& gemini_response,
    GeminiUsage* usage_out = nullptr);

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
