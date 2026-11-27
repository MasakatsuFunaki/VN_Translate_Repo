// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Gemini v1beta wire codec, transport Strategy interface, and PIMPL HTTPS/TLS
// client for `generativelanguage.googleapis.com:443`.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace crc::translate {

inline constexpr std::string_view kDefaultGeminiModel = "gemini-4-argon";
inline constexpr std::int64_t kDefaultThinkingBudget = 2048;
inline constexpr std::int64_t kDefaultMaxOutputTokens = 16384;
inline constexpr std::int64_t kGeminiMaxOutputTokens = 65536;
inline constexpr int kDefaultMaxApiRetries = 4;
inline constexpr int kDefaultRequestTimeoutSeconds = 240;
inline constexpr std::chrono::milliseconds kDefaultInitialBackoff{2000};

// Base exception for non-retryable or exhausted Gemini API failures.
class ApiError : public std::runtime_error {
public:
    explicit ApiError(const std::string& msg, int http_status = 0)
        : std::runtime_error(msg), http_status_(http_status) {}

    int http_status() const noexcept { return http_status_; }

private:
    int http_status_;
};

// Retryable rate-limit / quota-throttling error (HTTP 429 / RESOURCE_EXHAUSTED).
class RateLimitError : public ApiError {
public:
    explicit RateLimitError(const std::string& msg, int http_status = 429)
        : ApiError(msg, http_status) {}
};

// Retryable transient network or 5xx server failure.
class TransientApiError : public ApiError {
public:
    explicit TransientApiError(const std::string& msg, int http_status = 0)
        : ApiError(msg, http_status) {}
};

struct GeminiRequest {
    std::string system_prompt;
    std::string user_prompt;
    std::int64_t max_output_tokens = kDefaultMaxOutputTokens;
    std::int64_t thinking_budget = kDefaultThinkingBudget;
    // Optional discrete thinking level ("LOW", "MEDIUM", "HIGH"). When set,
    // emits `thinkingLevel` instead of `thinkingBudget` in `thinkingConfig`.
    std::string thinking_level;
    bool safety_block_none = true;
};

struct GeminiResponse {
    std::string text;
    std::string finish_reason;
    bool content_blocked = false;
    std::int64_t prompt_tokens = 0;
    std::int64_t cached_tokens = 0;
    std::int64_t output_tokens = 0;
    std::int64_t thought_tokens = 0;
};

// Serializes `request` into a Gemini `generateContent` JSON request body:
// - `systemInstruction.parts[0].text` iff `!request.system_prompt.empty()`
// - `contents[0]` with `role: "user"` and `parts[0].text = request.user_prompt`
// - `safetySettings` with `BLOCK_NONE` across all 5 configurable harm
//   categories when `request.safety_block_none == true`
// - `generationConfig.maxOutputTokens` clamped to `[1, kGeminiMaxOutputTokens]`
//   and `generationConfig.thinkingConfig` (`thinkingLevel` when non-empty,
//   otherwise `thinkingBudget` when >= 0).
std::string build_gemini_request_json(const GeminiRequest& request);

// Parses a Gemini `generateContent` HTTP response (`http_status` + `raw_json`):
// - Throws `RateLimitError` on HTTP 429 or `error.status == "RESOURCE_EXHAUSTED"`.
// - Throws `TransientApiError` on HTTP 5xx.
// - Throws `ApiError` on any other non-2xx HTTP status or malformed JSON.
// - On HTTP 2xx:
//   - Extracts `usageMetadata` (`promptTokenCount`, `cachedContentTokenCount`,
//     `candidatesTokenCount`, `thoughtsTokenCount`).
//   - Concatenates non-thought `candidates[0].content.parts[*].text`.
//   - Sets `content_blocked = true` (without throwing) when
//     `promptFeedback.blockReason` is present with missing/empty candidates OR
//     when `finishReason` is one of `SAFETY`, `PROHIBITED_CONTENT`, `BLOCKLIST`,
//     `RECITATION`, `SPII`, `IMAGE_SAFETY`.
GeminiResponse parse_gemini_response_json(std::string_view raw_json,
                                          int http_status = 200);

// Pure, side-effect-free `.env` parser supporting `KEY=VAL`, `export KEY=VAL`,
// `#` comments, UTF-8 BOM, and single/double quotes. Never mutates environment.
std::string parse_dotenv_value(std::string_view dotenv_contents,
                               std::string_view key_name);

// Pure Strategy interface for synchronous Gemini invocations.
class ITranslationTransport {
public:
    virtual ~ITranslationTransport();

    ITranslationTransport(const ITranslationTransport&) = delete;
    ITranslationTransport& operator=(const ITranslationTransport&) = delete;

    virtual GeminiResponse generate(const GeminiRequest& request) = 0;

protected:
    ITranslationTransport() = default;
    ITranslationTransport(ITranslationTransport&&) noexcept = default;
    ITranslationTransport& operator=(ITranslationTransport&&) noexcept = default;
};

struct GeminiClientOptions {
    std::string api_key;
    std::string model{kDefaultGeminiModel};
    int max_retries = kDefaultMaxApiRetries;
    int timeout_seconds = kDefaultRequestTimeoutSeconds;
    std::chrono::milliseconds initial_backoff = kDefaultInitialBackoff;
    std::function<void(std::chrono::milliseconds)> sleep_fn;
};

// Thread-compatible PIMPL HTTPS/TLS client for `generativelanguage.googleapis.com:443`.
// Loads the Windows `ROOT` certificate store once into an internal RAII SSL
// context and reuses it across requests.
class GeminiClient final : public ITranslationTransport {
public:
    explicit GeminiClient(GeminiClientOptions options = {});
    ~GeminiClient() override;

    GeminiClient(GeminiClient&&) noexcept;
    GeminiClient& operator=(GeminiClient&&) noexcept;

    GeminiClient(const GeminiClient&) = delete;
    GeminiClient& operator=(const GeminiClient&) = delete;

    GeminiResponse generate(const GeminiRequest& request) override;

    // Resolves `GEMINI_API_KEY` from process environment (`_wgetenv`), then
    // `%USERPROFILE%\.gemini\.env` (or `dotenv_override` when non-empty), then
    // HKCU\Environment without mutating process environment or logging keys.
    static std::string resolve_gemini_api_key(
        const std::filesystem::path& dotenv_override = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace crc::translate
