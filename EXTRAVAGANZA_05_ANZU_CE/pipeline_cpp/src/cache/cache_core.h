// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Batch translation driver and cache management for WelcomeToAnzU_CE.
#pragma once

#include "cache/gemini_client.h"
#include "cache/glossary.h"
#include "vn/llm/llm_types.h"
#include "vn/llm/zero_thought_guard.h"

#include <boost/json.hpp>

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

namespace exm::translate {

inline constexpr int kDefaultBatchSize = vn::llm::kDefaultBatchSize;
inline constexpr std::size_t CACHE_DISCARD_THRESHOLD = 100;

inline constexpr std::int64_t kMinOutputTokens = vn::llm::kMinOutputTokensFloor;
inline constexpr std::int64_t kMaxOutputTokens = vn::llm::kMaxOutputTokensCeiling;
inline constexpr std::int64_t kTokensPerLineEstimate =
    vn::llm::kDefaultTokensPerBatchLine;
inline constexpr std::size_t kZeroThoughtGuardMinLines =
    vn::llm::kMinInitialBatchSliceForThoughtCheck;
inline constexpr const char* kDefaultThinkingLevel = "MEDIUM";
static_assert(std::string_view(kDefaultThinkingLevel) ==
              vn::llm::kDefaultThinkingLevel);

// Thrown when a guarded Gemini 4 model call (`carbon`, `neon`, `hydrogen`)
// configured with thinkingLevel `"MEDIUM"` or `"HIGH"` completes without
// content blocking on a qualifying batch yet reports thought_tokens <= 0.
class ZeroThoughtError : public vn::llm::LlmZeroThoughtError {
public:
    using vn::llm::LlmZeroThoughtError::LlmZeroThoughtError;
};

struct GeminiUsageTelemetry {
    std::int64_t prompt_tokens = 0;
    std::int64_t candidates_tokens = 0;
    std::int64_t thought_tokens = 0;
    std::int64_t cached_tokens = 0;
    std::int64_t total_tokens = 0;
};

struct GeminiParsedResponse {
    std::string candidate_text;
    GeminiUsageTelemetry usage;
    bool blocked = false;
};

struct GeminiBatchResult {
    std::map<int, std::string> translations;
    GeminiUsageTelemetry usage;
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

std::string build_user_prompt(
    const std::vector<std::pair<std::string, std::string>>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context,
    const std::vector<std::pair<std::string, std::string>>& context_after = {});

std::map<int, std::string> parse_numbered_response(const std::string& response_text);

// Computes batch_line_count * kTokensPerLineEstimate without signed overflow,
// clamped to [kMinOutputTokens, kMaxOutputTokens] ([16384, 65536]).
std::int64_t compute_max_output_tokens(std::size_t batch_line_count) noexcept;

// Builds the Gemini generateContent JSON request payload:
//   - systemInstruction.parts[0].text == SYSTEM_PROMPT
//   - contents[0] == {role: "user", parts: [{text: build_user_prompt(...)}]}
//   - generationConfig.maxOutputTokens == compute_max_output_tokens(batch_lines.size())
//   - generationConfig.thinkingConfig == {"thinkingLevel": thinking_level}
//     (when thinking_level is non-empty; never emits "thinkingBudget")
//   - safetySettings == all 5 HARM_CATEGORY_* entries set to "BLOCK_NONE":
//     HARM_CATEGORY_HARASSMENT, HARM_CATEGORY_HATE_SPEECH,
//     HARM_CATEGORY_SEXUALLY_EXPLICIT, HARM_CATEGORY_DANGEROUS_CONTENT,
//     HARM_CATEGORY_CIVIC_INTEGRITY.
boost::json::object build_gemini_request(
    const std::vector<std::pair<std::string, std::string>>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context = {},
    const std::vector<std::pair<std::string, std::string>>& context_after = {},
    std::string_view thinking_level = kDefaultThinkingLevel);

// Parses a Gemini generateContent JSON response:
//   - Concatenates candidates[0].content.parts[*].text in order while excluding
//     any part where "thought" is boolean true.
//   - Extracts usageMetadata.{promptTokenCount, candidatesTokenCount,
//     thoughtsTokenCount, cachedContentTokenCount, totalTokenCount} (accepting
//     both int64 and uint64 JSON integers; missing/non-integer fields -> 0).
//   - Marks blocked = true iff promptFeedback.blockReason is a non-empty string
//     other than "BLOCK_REASON_UNSPECIFIED", OR any candidate object in
//     candidates has finishReason in {"SAFETY", "PROHIBITED_CONTENT",
//     "BLOCKLIST", "SPII", "RECITATION", "IMAGE_SAFETY"}.
GeminiParsedResponse parse_gemini_response(const boost::json::value& response);

// Throws ZeroThoughtError iff `vn::llm::is_guarded_thinking_mode(model, thinking_level)`
// (`carbon`, `neon`, or `hydrogen` at `"MEDIUM"` or `"HIGH"`), !blocked,
// batch_line_count >= kZeroThoughtGuardMinLines (4), and thought_tokens <= 0.
void enforce_thinking_guard(std::string_view model,
                            std::string_view thinking_level,
                            bool blocked,
                            std::size_t batch_line_count,
                            std::int64_t thought_tokens);

GeminiBatchResult call_gemini_detailed(
    gemini::Client& client,
    const std::vector<std::pair<std::string, std::string>>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context = {},
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

int translate_all(const TranslateOptions& opt);

}  // namespace exm::translate
