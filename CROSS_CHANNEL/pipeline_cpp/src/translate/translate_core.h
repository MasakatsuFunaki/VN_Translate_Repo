// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Batch translation driver, insertion-ordered cache, and postprocessing for
// CROSS†CHANNEL -FINAL COMPLETE-.
//
// Follows the canonical 4-file `src/translate/` layout (`gemini_client`,
// `glossary`, `speaker_gate`, `translate_core`) shared across Argon games:
//   - `translate_all(TranslateOptions)` preserves offline cache-only table
//     rebuilding (`02_build_tsv.exe` / `00_run_all.exe`).
//   - `BatchTranslator` / `run_gemini_translation()` drives live `gemini-4-argon`
//     batched translation (`02_translate.exe`) with rolling `<previous_context>`,
//     `<context_after>` lookahead, `neutral_system_prompt()` refusal fallback,
//     recursive sub-batch bisection down to size 1, atomic cache +
//     `blocked_lines.json` + `last_gemini_translate/last_translation.json`
//     persistence, and a client-side `--max-budget-eur` circuit breaker.
#pragma once

#include "translate/gemini_client.h"
#include "translate/glossary.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/json.hpp>

namespace crc::translate {

inline constexpr int kExitOk = 0;
inline constexpr int kExitFailure = 1;
inline constexpr int kExitGateOrRefused = 2;
inline constexpr std::size_t CACHE_DISCARD_THRESHOLD = 100;

// Insertion-ordered jp -> en cache.  Insertion order decides the key order of
// `translation_cache_gemini.json`, so resuming appends without reshuffling.
class Cache {
public:
    bool contains(const std::string& jp) const { return index_.count(jp) != 0; }
    const std::string* get(const std::string& jp) const {
        auto it = index_.find(jp);
        return it == index_.end() ? nullptr : &order_[it->second].second;
    }
    void set(const std::string& jp, const std::string& en) {
        auto it = index_.find(jp);
        if (it != index_.end()) {
            order_[it->second].second = en;
        } else {
            index_.emplace(jp, order_.size());
            order_.emplace_back(jp, en);
        }
    }
    void set_default(const std::string& jp, const std::string& en) {
        if (!contains(jp)) {
            set(jp, en);
        }
    }
    std::size_t size() const { return order_.size(); }
    const std::vector<std::pair<std::string, std::string>>& items() const { return order_; }

private:
    std::vector<std::pair<std::string, std::string>> order_;
    std::map<std::string, std::size_t> index_;
};

// Kana + CJK only (excludes punctuation-only symbol lines).
bool has_real_japanese(const std::string& text);
bool needs_translation(const std::string& text);

// Indices into a script object's `strings` array worth translating.
std::vector<std::size_t> translatable_indices(const boost::json::object& fdata);

// Speaker for one entry object: `speaker` field (or legacy `Speaker\r\n` prefix),
// mapped through `name_translations()`, or `"NARRATION"` when absent.
std::string extract_speaker(const boost::json::object& s);

// Replace common typographic non-ASCII punctuation with CP932-safe ASCII.
std::string sanitize_ascii(const std::string& text);

// Re-attach the English speaker plate (for legacy `Speaker\r\n` entries) and
// restore a trailing CRLF when present on `original`.
std::string postprocess(const std::string& original, const std::string& translated);

// Clean a raw LLM translation line for `jp`: strips accidental `[Speaker]` tags,
// unescapes `\\r\\n` -> `\r\n` then `\\n` -> `\n` (preserving `%N` verbatim),
// normalizes extended punctuation/ruby markup (`―`/`─` -> `--`, `⋯` -> `...`,
// `「`/`」` -> `"`, `『`/`』` -> `'`, `〜`/`～` -> `~`, `é`/`ê`/`è` -> `e`,
// `[base|ruby]` -> `ruby`), and applies `postprocess(jp, ...)`.
std::string clean_translated_line(const std::string& jp, const std::string& raw_en);

Cache load_cache(const std::string& cache_file);
void save_cache(const Cache& cache, const std::string& cache_file);
std::size_t cache_entry_count(const std::string& cache_file);

std::optional<std::string> refuse_cache_discard(std::size_t cache_entries,
                                                const std::string& flag,
                                                bool discard_cache);

bool is_failed_entry(const std::string& jp, const std::string& en);
std::size_t purge_failed_entries(Cache& cache);

// Legacy permissive parser retained for compatibility.
std::map<int, std::string> parse_numbered_response(const std::string& response_text);

// --- Live Gemini batch-translation pure helpers & contracts -----------------

struct PromptLine {
    std::string speaker;
    std::string jp;
    std::int64_t offset = 0;
};

struct ParsedNumberedAnswer {
    std::map<int, std::string> entries;
    int rejected = 0;
    std::string final_tail;
};

// Clamps `batch_size * 300` into `[kDefaultMaxOutputTokens, kGeminiMaxOutputTokens]`.
std::int64_t compute_max_output_tokens(int batch_size);

// Escapes `\r\n` -> `\\r\\n` and `\n` -> `\\n` for single-line prompt framing.
std::string escape_prompt_line(const std::string& text);

// Builds user prompt with optional `<previous_context>` (last `context_window`
// `[Speaker] text` pairs), numbered `<lines_to_translate>`, and optional
// `<context_after>` (first `context_after_window` upcoming lines).
std::string build_batch_user_prompt(
    const std::vector<PromptLine>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context,
    const std::vector<PromptLine>& context_after = {},
    int context_window = kDefaultContextWindow,
    int context_after_window = kDefaultContextAfterWindow);

// Strict numbered-response parser: accepts ASCII & fullwidth digits (`1.` / `１．`)
// and markdown emphasis (`**1.**`), joins wrapped continuation lines with `' '`,
// rejects duplicate, non-increasing, out-of-range (`> expected`), or malformed
// openers (`rejected > 0` clears `entries`), strips `[Speaker]` tags, and
// unescapes `\\r\\n` / `\\n` while keeping `%N` verbatim.
ParsedNumberedAnswer parse_numbered_response_strict(const std::string& response_text,
                                                    int expected);

// Converts a `GeminiResponse` into usable `{1..requested -> text}` translations:
// - Returns empty map when `response.content_blocked == true` or `rejected > 0`.
// - When `response.finish_reason == "MAX_TOKENS"`, drops the final parsed entry
//   (truncated fragment) and returns the earlier complete entries.
// - Otherwise requires `entries.size() == requested` (returns empty map on count
//   mismatch so line numbering can never silently shift).
std::map<int, std::string> extract_usable_translations(
    const GeminiResponse& response,
    std::size_t requested,
    std::string* warning_out = nullptr);

// Validates a post-cleaned English line against `jp`: rejects empty output,
// unchanged JP echo (`en == jp` when `jp` has non-ASCII), leaked Japanese
// kana/kanji in `en`, or mismatched `%N` occurrence count when `jp` has `%N`.
bool validate_line_translation(const std::string& jp,
                               const std::string& en,
                               std::string* reason_out = nullptr);

struct TokenUsage {
    std::int64_t api_calls = 0;
    std::int64_t prompt_tokens = 0;
    std::int64_t cached_tokens = 0;
    std::int64_t output_tokens = 0;
    std::int64_t thought_tokens = 0;
};

// Estimates spend in USD / EUR (`0.95` conservative EUR/USD multiplier).
// `gemini-4-argon`: $2.00/1M uncached input, $0.20/1M cached input, $10.00/1M
// output+thought. `gemini-3.1-pro*`: $2.00 / $0.20 / $12.00 per 1M.
double estimate_cost_usd(const TokenUsage& usage, const std::string& model);
double estimate_cost_eur(const TokenUsage& usage, const std::string& model);

struct BlockedLine {
    std::int64_t offset = 0;
    std::string speaker;
    std::string jp;
    std::string reason;
};

// Offline cache-apply options (`02_build_tsv.exe`).
struct TranslateOptions {
    int test_batches = 0;
    std::string input_file;     // extracted_text.json
    std::string cache_file;     // translation_cache_gemini.json
    std::string output_file;    // translated_text.json
    std::string test_dir;
    std::string tsv_file;       // build\install\game\translations.tsv
    std::string game_tsv_file;  // <game-dir>\translations.tsv
};

// Offline cache builder (`02_build_tsv.exe`): speaker gate -> apply existing
// cache -> write `translated_text.json` -> build `translations.tsv`.
// Throws `std::runtime_error` if any translatable string is missing from cache.
int translate_all(const TranslateOptions& opt);

// Live Gemini batch-translation options (`02_translate.exe`).
struct GeminiTranslateOptions {
    std::string input_file;       // extracted_text.json
    std::string cache_file;       // translation_cache_gemini.json
    std::string output_file;      // translated_text.json
    std::string tsv_file;         // build\install\game\translations.tsv
    std::string game_tsv_file;    // optional <game-dir>\translations.tsv
    std::string blocked_file;     // script_output\blocked_lines.json
    std::string raw_log_file;     // script_output\gemini_raw_answers.log
    std::string last_batch_file;  // script_output\last_gemini_translate\last_translation.json
    std::string model{kDefaultModel};
    int batch_size = kDefaultBatchSize;
    int context_window = kDefaultContextWindow;
    int context_after_window = kDefaultContextAfterWindow;
    std::int64_t thinking_budget = kDefaultThinkingBudget;
    std::string thinking_level;
    int max_batches = 0;          // 0 = unlimited
    double max_budget_eur = 50.0; // <= 0 disables budget circuit breaker
    bool gate_only = false;
    bool retranslate = false;
    bool discard_cache = false;
    bool skip_tsv_on_partial = true;
};

struct TranslateReport {
    int exit_code = kExitOk;
    std::size_t total_translatable = 0;
    std::size_t unique_translatable = 0;
    std::size_t translated_new = 0;
    std::size_t cached_total = 0;
    std::size_t blocked_count = 0;
    std::size_t remaining_uncached = 0;
    bool stopped_by_max_batches = false;
    bool stopped_by_budget = false;
    TokenUsage usage;
    double estimated_usd = 0.0;
    double estimated_eur = 0.0;
};

std::optional<std::string> validate_gemini_options(const GeminiTranslateOptions& opt);

// Thread-compatible PIMPL orchestrator for batched Gemini translation.
class BatchTranslator {
public:
    explicit BatchTranslator(ITranslationTransport* transport_override = nullptr);
    ~BatchTranslator();

    BatchTranslator(BatchTranslator&&) noexcept;
    BatchTranslator& operator=(BatchTranslator&&) noexcept;

    BatchTranslator(const BatchTranslator&) = delete;
    BatchTranslator& operator=(const BatchTranslator&) = delete;

    TranslateReport run(const GeminiTranslateOptions& opt);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace crc::translate
