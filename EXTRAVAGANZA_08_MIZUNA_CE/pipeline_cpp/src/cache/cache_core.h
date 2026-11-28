// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Batch translation driver and cache management for EXTRA_VA_MIZUNA_CE.
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
inline constexpr std::int64_t kMinOutputTokens = vn::llm::kMinOutputTokensFloor;
inline constexpr std::int64_t kMaxOutputTokens = vn::llm::kMaxOutputTokensCeiling;
inline constexpr std::int64_t kTokensPerLineEstimate = vn::llm::kDefaultTokensPerBatchLine;
inline constexpr std::size_t kZeroThoughtGuardMinBatchLines =
    vn::llm::kMinInitialBatchSliceForThoughtCheck;
inline constexpr std::string_view kDefaultThinkingLevel = vn::llm::kDefaultThinkingLevel;

// Raised when an unblocked guarded Gemini 4 batch (e.g. gemini-4-carbon at
// MEDIUM or HIGH thinking) of at least kZeroThoughtGuardMinBatchLines lines
// reports thought_tokens <= 0.
class ZeroThoughtError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct UsageTelemetry {
    std::int64_t prompt_tokens = 0;
    std::int64_t candidates_tokens = 0;
    std::int64_t thought_tokens = 0;
    std::int64_t cached_tokens = 0;
    std::int64_t total_tokens = 0;
};

struct ParsedGeminiResponse {
    std::string text;
    UsageTelemetry usage;
    bool blocked = false;
    std::string finish_reason;
    std::string block_reason;
};

struct GeminiBatchResult {
    std::map<int, std::string> translations;
    UsageTelemetry usage;
    bool blocked = false;
};

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

// Formats the user turn text containing optional
// "<previous_context>\n...\n</previous_context>" (emitted iff previous_context
// is non-empty, up to CONTEXT_WINDOW entries), numbered
// "<lines_to_translate>\n...\n</lines_to_translate>", optional
// "<context_after>\n...\n</context_after>" (emitted iff context_after is
// non-empty, up to CONTEXT_AFTER entries), and a trailing instruction paragraph
// referencing <lines_to_translate>, <previous_context>, and <context_after>.
std::string build_user_prompt(
    const std::vector<std::pair<std::string, std::string>>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context,
    const std::vector<std::pair<std::string, std::string>>& context_after = {});

// Builds the Gemini generateContent JSON request object:
//   - rejects disallowed models (e.g. any "argon" identifier) via
//     `vn::llm::ensure_supported_model(model)`
//   - systemInstruction.parts[0].text == SYSTEM_PROMPT
//   - contents[0] == {role: "user", parts: [{text: build_user_prompt(...)}]}
//   - generationConfig.maxOutputTokens == clamp(batch_lines.size() * 300, 16384, 65536)
//     (overflow-safe for arbitrary batch_lines.size())
//   - generationConfig.thinkingConfig == {"thinkingLevel": thinking_level} when
//     thinking_level is non-empty; never emits "thinkingBudget"
//   - safetySettings == 5 entries with threshold "BLOCK_NONE" covering:
//     HARM_CATEGORY_HARASSMENT, HARM_CATEGORY_HATE_SPEECH,
//     HARM_CATEGORY_SEXUALLY_EXPLICIT, HARM_CATEGORY_DANGEROUS_CONTENT,
//     HARM_CATEGORY_CIVIC_INTEGRITY
boost::json::object build_gemini_request_body(
    const std::vector<std::pair<std::string, std::string>>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context = {},
    const std::vector<std::pair<std::string, std::string>>& context_after = {},
    std::string_view thinking_level = kDefaultThinkingLevel,
    std::string_view model = MODEL);

// Parses a Gemini generateContent JSON response:
//   - Concatenates candidates[0].content.parts[*].text while excluding any part
//     where part.thought is boolean true (thought == false or omitted is kept).
//   - Reads non-negative integer counts (int64 or uint64; negative/non-integer/missing -> 0)
//     from usageMetadata.{promptTokenCount, candidatesTokenCount, thoughtsTokenCount,
//     cachedContentTokenCount, totalTokenCount}.
//   - Sets block_reason from promptFeedback.blockReason and finish_reason from
//     candidates[0].finishReason.
//   - Sets blocked = true iff block_reason is non-empty and != "BLOCK_REASON_UNSPECIFIED",
//     or finish_reason is one of {"SAFETY", "RECITATION", "BLOCKLIST",
//     "PROHIBITED_CONTENT", "SPII", "IMAGE_SAFETY"}.
ParsedGeminiResponse parse_gemini_response(const boost::json::value& response);

// Throws ZeroThoughtError iff `vn::llm::is_guarded_thinking_mode(model, thinking_level)`
// holds (supported Gemini 4 reasoning family `carbon`/`neon`/`hydrogen` at
// `"MEDIUM"` or `"HIGH"`), !blocked,
// batch_line_count >= kZeroThoughtGuardMinBatchLines (4), and thought_tokens <= 0.
void enforce_zero_thought_guard(std::string_view model,
                                std::string_view thinking_level,
                                bool blocked,
                                std::size_t batch_line_count,
                                std::int64_t thought_tokens);

std::map<int, std::string> parse_numbered_response(const std::string& response_text);

// Executes one generateContent request, parses text + usage telemetry, enforces
// enforce_zero_thought_guard(...), and parses 1-based numbered translations.
GeminiBatchResult call_gemini_detailed(
    gemini::Client& client,
    const std::vector<std::pair<std::string, std::string>>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context = {},
    const std::vector<std::pair<std::string, std::string>>& context_after = {},
    const std::string& model = MODEL,
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
    UsageTelemetry* total_usage = nullptr;
};

// Runs speaker checks (requiring "dialogue" entries to carry mapped
// Speaker\r\n「...」 prefixes while speakerless prose uses "narrative"), seeds
// cache with name_translations_ordered(), translates uncached lines in story
// order (forwarding chronologically accurate <previous_context> and
// <context_after> lookahead), propagates ZeroThoughtError immediately before
// mutating cache_file or output_file, and writes output_file + tsv_file.
int translate_all(const TranslateOptions& opt);

}  // namespace exm::translate
