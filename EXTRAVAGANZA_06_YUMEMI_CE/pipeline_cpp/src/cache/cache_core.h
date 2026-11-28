// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Batch translation driver and cache management for YUMEMIRYOJOKU_CE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/json.hpp>

#include "cache/gemini_client.h"
#include "cache/glossary.h"
#include "vn/llm/llm_types.h"
#include "vn/llm/zero_thought_guard.h"

namespace exm::translate {

inline constexpr int kDefaultBatchSize = vn::llm::kDefaultBatchSize;
inline constexpr std::size_t CACHE_DISCARD_THRESHOLD = 100;
inline constexpr std::int64_t kTokensPerLineEstimate =
    vn::llm::kDefaultTokensPerBatchLine;
inline constexpr std::int64_t kMinMaxOutputTokens =
    vn::llm::kMinOutputTokensFloor;
inline constexpr std::int64_t kMaxMaxOutputTokens =
    vn::llm::kMaxOutputTokensCeiling;
inline constexpr std::size_t kZeroThoughtGuardMinBatchLines =
    vn::llm::kMinInitialBatchSliceForThoughtCheck;
inline constexpr std::string_view kDefaultThinkingLevel =
    vn::llm::kDefaultThinkingLevel;

struct UsageTelemetry {
    std::int64_t input_tokens = 0;
    std::int64_t output_tokens = 0;
    std::int64_t thought_tokens = 0;
    std::int64_t cached_tokens = 0;
};

struct ParsedGeminiResponse {
    std::string candidate_text;
    UsageTelemetry usage;
    bool blocked = false;
    std::string block_reason;
};

struct GeminiBatchResult {
    std::map<int, std::string> translations;
    UsageTelemetry usage;
    bool blocked = false;
};

using ZeroThoughtError = vn::llm::LlmZeroThoughtError;

// Insertion-ordered jp -> en cache hiding private data via PIMPL.
class Cache {
public:
    Cache();
    ~Cache();

    Cache(const Cache& other);
    Cache& operator=(const Cache& other);
    Cache(Cache&& other) noexcept;
    Cache& operator=(Cache&& other) noexcept;

    bool contains(const std::string& jp) const;
    const std::string* get(const std::string& jp) const;
    void set(const std::string& jp, const std::string& en);
    std::size_t size() const;
    const std::vector<std::pair<std::string, std::string>>& items() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::optional<std::string> validate_options(int batch_size, int test_n, int max_batches);

bool has_real_japanese(const std::string& text);
bool needs_translation(const std::string& text);
std::vector<std::size_t> translatable_indices(const boost::json::object& fdata);

std::string extract_speaker(const std::string& text);
std::string sanitize_ascii(const std::string& text);
std::string postprocess(const std::string& original, const std::string& translated);
std::string strip(const std::string& s);

Cache load_cache(const std::string& cache_file);
void save_cache(const Cache& cache, const std::string& cache_file);
std::size_t cache_entry_count(const std::string& cache_file);

std::optional<std::string> refuse_cache_discard(std::size_t cache_entries,
                                                const std::string& flag,
                                                bool discard_cache);

bool is_failed_entry(const std::string& jp, const std::string& en);
std::size_t purge_failed_entries(Cache& cache);

std::string build_user_prompt(
    const std::vector<std::pair<std::string, std::string>>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context,
    const std::vector<std::pair<std::string, std::string>>& context_after = {});

std::map<int, std::string> parse_numbered_response(const std::string& response_text);

// Computes generationConfig.maxOutputTokens as (batch_line_count * 300)
// clamped to [16384, 65536] without integer overflow for any batch_line_count.
std::int64_t compute_max_output_tokens(std::size_t batch_line_count) noexcept;

// Builds the Gemini generateContent JSON request object:
// - "systemInstruction": {"parts": [{"text": SYSTEM_PROMPT}]}
// - "contents": [{"role": "user", "parts": [{"text": build_user_prompt(...)}]}]
// - "generationConfig": {
//     "maxOutputTokens": compute_max_output_tokens(batch_lines.size()),
//     "thinkingConfig": {"thinkingLevel": std::string(thinking_level)}
//   }  (never emits "thinkingBudget")
// - "safetySettings": 5 entries with "threshold": "BLOCK_NONE" for categories:
//   HARM_CATEGORY_HARASSMENT, HARM_CATEGORY_HATE_SPEECH,
//   HARM_CATEGORY_SEXUALLY_EXPLICIT, HARM_CATEGORY_DANGEROUS_CONTENT,
//   HARM_CATEGORY_CIVIC_INTEGRITY.
boost::json::object build_gemini_request(
    const std::vector<std::pair<std::string, std::string>>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context,
    const std::vector<std::pair<std::string, std::string>>& context_after = {},
    std::string_view thinking_level = kDefaultThinkingLevel);

// Parses a Gemini generateContent JSON response:
// - Concatenates string "text" fields from candidates[0].content.parts[*],
//   skipping any part object where "thought" is boolean true.
// - Extracts integer token counts from usageMetadata:
//   promptTokenCount -> input_tokens,
//   candidatesTokenCount -> output_tokens,
//   thoughtsTokenCount -> thought_tokens,
//   cachedContentTokenCount -> cached_tokens
//   (accepting signed int64 or unsigned uint64 JSON integers, defaulting missing
//   or non-integer fields to 0, clamping uint64 > INT64_MAX to INT64_MAX).
// - Sets blocked = true (and populates block_reason) if promptFeedback.blockReason
//   is a non-empty string other than "BLOCK_REASON_UNSPECIFIED", or if
//   candidates[0].finishReason is one of: "SAFETY", "RECITATION", "BLOCKLIST",
//   "PROHIBITED_CONTENT", "SPII", "IMAGE_SAFETY", "LANGUAGE", "OTHER".
//   Normal finish reasons ("STOP", "MAX_TOKENS", "FINISH_REASON_UNSPECIFIED",
//   or absent) leave blocked = false.
ParsedGeminiResponse parse_gemini_response(const boost::json::value& response);

// Throws ZeroThoughtError if all of the following hold:
// - `vn::llm::is_guarded_thinking_mode(model, thinking_level)` is true
//   (`carbon`, `neon`, or `hydrogen` with thinkingLevel `"MEDIUM"` or `"HIGH"`),
// - !blocked,
// - batch_line_count >= kZeroThoughtGuardMinBatchLines (4), and
// - thought_tokens <= 0.
void enforce_zero_thought_guard(std::string_view model,
                                std::string_view thinking_level,
                                bool blocked,
                                std::size_t batch_line_count,
                                std::int64_t thought_tokens);

// Rejects disallowed models (`vn::llm::ensure_supported_model`, e.g. `"argon"`),
// builds the Gemini request, invokes client.generate_content(model, body),
// parses text and UsageTelemetry, enforces enforce_zero_thought_guard, and
// parses numbered translations (prepending "1. " when a non-empty single-line
// answer omits a leading "1." / "1)" / "1:" prefix).
GeminiBatchResult call_gemini_detailed(
    gemini::Client& client,
    const std::vector<std::pair<std::string, std::string>>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context,
    const std::vector<std::pair<std::string, std::string>>& context_after = {},
    std::string_view model = MODEL,
    std::string_view thinking_level = kDefaultThinkingLevel);

std::map<int, std::string> call_gemini(
    gemini::Client& client,
    const std::vector<std::pair<std::string, std::string>>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context,
    const std::vector<std::pair<std::string, std::string>>& context_after = {});

struct TranslateOptions {
    int batch_size = kDefaultBatchSize;
    bool retranslate = false;
    std::optional<std::string> only_file;
    int test_batches = 0;
    int max_batches = 0;
    std::string input_file;
    std::string cache_file;
    std::string output_file;
    std::string test_dir;
    std::string tsv_file;
    gemini::LazyClient::Factory client_factory;
};

// Runs speaker gate, translates uncached lines in story order, updates
// cache_file / output_file, and writes tsv_file. Propagates ZeroThoughtError
// immediately without mutating cache_file, output_file, or tsv_file when a
// batch violates enforce_zero_thought_guard.
int translate_all(const TranslateOptions& opt);

}  // namespace exm::translate
