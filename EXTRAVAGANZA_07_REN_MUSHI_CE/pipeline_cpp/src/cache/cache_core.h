// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Batch translation driver, thin `vn::llm` wire adapter, and cache management
// for Ren_mushi_CE.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
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
inline constexpr std::int64_t kTokensPerLineEstimate = vn::llm::kDefaultTokensPerBatchLine;
inline constexpr std::int64_t kMinOutputTokens = vn::llm::kMinOutputTokensFloor;
inline constexpr std::int64_t kMaxOutputTokens = vn::llm::kMaxOutputTokensCeiling;
inline constexpr std::size_t kZeroThoughtGuardMinLines =
    vn::llm::kMinInitialBatchSliceForThoughtCheck;
inline constexpr std::string_view kDefaultThinkingLevel = vn::llm::kDefaultThinkingLevel;

using ZeroThoughtError = vn::llm::LlmZeroThoughtError;

struct GeminiUsageTelemetry {
    std::int64_t prompt_tokens = 0;
    std::int64_t candidate_tokens = 0;
    std::int64_t cached_tokens = 0;
    std::int64_t thought_tokens = 0;
    bool blocked = false;
};

struct ParsedGeminiResponse {
    std::string candidate_text;
    GeminiUsageTelemetry telemetry;
};

struct GeminiBatchResult {
    std::map<int, std::string> translations;
    GeminiUsageTelemetry telemetry;
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

// Builds a Gemini generateContent request payload via `vn::llm`:
//   - rejects disallowed models (e.g. any "argon" identifier) via
//     `vn::llm::ensure_supported_model(model)`
//   - generationConfig.maxOutputTokens = clamp(batch_lines.size() * 300, 16384, 65536)
//   - generationConfig.thinkingConfig = {"thinkingLevel": thinking_level} (no thinkingBudget)
//   - safetySettings = 5 BLOCK_NONE categories (HARASSMENT, HATE_SPEECH,
//     SEXUALLY_EXPLICIT, DANGEROUS_CONTENT, CIVIC_INTEGRITY)
boost::json::object build_gemini_request(
    const std::vector<std::pair<std::string, std::string>>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context = {},
    const std::vector<std::pair<std::string, std::string>>& context_after = {},
    std::string_view thinking_level = kDefaultThinkingLevel,
    std::string_view model = MODEL);

// Extracts concatenated non-thought candidate text (skipping parts with
// thought == true) and usageMetadata token counts (including thoughtsTokenCount)
// via `vn::llm::parse_gemini_response_json`.
ParsedGeminiResponse parse_gemini_response(const boost::json::value& response);

// Throws ZeroThoughtError when `vn::llm::is_guarded_thinking_mode(model, thinking_level)`
// holds (supported Gemini 4 reasoning family `carbon`/`neon`/`hydrogen` at
// `"MEDIUM"` or `"HIGH"`), !telemetry.blocked, batch_line_count >= 4, and
// telemetry.thought_tokens <= 0.
void enforce_thought_telemetry_guard(std::string_view model,
                                     std::string_view thinking_level,
                                     std::size_t batch_line_count,
                                     const GeminiUsageTelemetry& telemetry);

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
    gemini::LazyClient::Factory client_factory{};
};

// Runs speaker gate, merges name_translations_ordered() plus newly translated
// batches into cache_file, writes output_file (translated_text.json), and
// builds tsv_file (CP932 translation_table.tsv). Propagates ZeroThoughtError
// before writing cache_file, output_file, or tsv_file.
int translate_all(const TranslateOptions& opt);

}  // namespace exm::translate
