// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Batch translation driver (pipeline step 2).
//
// Reads extracted_text.json, translates in batches with a rolling
// per-script context window, caches every successful API call
// (resume-safe), and materialises translated_text.json where every entry
// gains a 'translated' field.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/json.hpp>

#include "anthropic_client.h"

namespace snu::translate {

// Insertion-ordered jp -> en cache; order decides the JSON key order.
class Cache {
public:
    bool contains(const std::string& jp) const { return index_.count(jp) != 0; }
    const std::string* get(const std::string& jp) const {
        auto it = index_.find(jp);
        return it == index_.end() ? nullptr : &order_[it->second].second;
    }
    void set(const std::string& jp, const std::string& en) {
        auto it = index_.find(jp);
        if (it != index_.end()) order_[it->second].second = en;
        else {
            index_.emplace(jp, order_.size());
            order_.emplace_back(jp, en);
        }
    }
    void set_default(const std::string& jp, const std::string& en) {
        if (!contains(jp)) set(jp, en);
    }
    std::size_t size() const { return order_.size(); }
    const std::vector<std::pair<std::string, std::string>>& items() const { return order_; }

private:
    std::vector<std::pair<std::string, std::string>> order_;
    std::map<std::string, std::size_t> index_;
};

// True when the text contains hiragana / katakana / CJK.
bool needs_translation(const std::string& text);

// Strip a [Speaker] tag the model echoed at the start of its answer.
std::string strip_speaker_tags(const std::string& text);

// max_tokens for a batch, clamped to the model's ceiling.
std::int64_t compute_max_tokens(int batch_size);

// Returns an error string if the option combination would spend money
// unintentionally; nullopt when OK.
std::optional<std::string> validate_options(int batch_size, int test_n, int max_batches);

// Trim leading/trailing ASCII whitespace (space, \t, \r, \n, \f, \v).
std::string strip(const std::string& s);

// Encode real line breaks as literal "\n" for the numbered wire format.
std::string escape_line_breaks(const std::string& s);

// Turn "\n" escapes back into real breaks; strips stray ASCII blanks around them.
std::string unescape_line_breaks(const std::string& s);

// Speaker tag for a line: "NAME", the glossary name, or "NARRATION".
std::string extract_speaker(const boost::json::object& entry);

Cache load_cache(const std::string& cache_file);
void save_cache(const Cache& cache, const std::string& cache_file);

// Entries in the cache file, or 0 when there is no file.
std::size_t cache_entry_count(const std::string& cache_file);

// Cache-delete flags require --discard-cache above this line count.
inline constexpr std::size_t CACHE_DISCARD_THRESHOLD = 100;

// Guard against accidental cache deletion. Returns the refusal reason,
// or nullopt when the caller may proceed.
std::optional<std::string> refuse_cache_discard(std::size_t cache_entries,
                                                const std::string& flag,
                                                bool discard_cache);

// True if en == jp and jp has non-ASCII bytes (a failed translation echo).
bool is_failed_entry(const std::string& jp, const std::string& en);

// Remove failed entries so they re-queue. Returns how many were removed.
std::size_t purge_failed_entries(Cache& cache);

// Cache one translated line. Empty `en` leaves it uncached so it re-queues.
void apply_line_result(Cache& cache,
                       std::vector<std::pair<std::string, std::string>>& previous_context,
                       const std::string& speaker, const std::string& jp,
                       const std::string& en);

// Build the user prompt for one batch.
std::string build_user_prompt(
    const std::vector<std::pair<std::string, std::string>>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context);

// Parse Claude's "1. ... 2. ..." numbered response into {line_no -> text}.
// One rejected line empties the whole map so the batch re-queues.
// `expected` = 0 disables the upper-bound check.
std::map<int, std::string> parse_numbered_response(const std::string& response_text,
                                                   int expected = 0,
                                                   int* rejected_lines = nullptr,
                                                   std::string* final_entry_tail = nullptr);

// True if the parsed count matches the requested count (or the answer was
// truncated by max_tokens, where a short count is expected).
bool answer_count_is_usable(std::size_t requested, std::size_t parsed,
                            const std::string& stop_reason);

// Remove the last entry when the answer was cut mid-sentence by max_tokens.
bool drop_truncated_entry(std::map<int, std::string>& translations,
                          const std::string& stop_reason);

// Clear everything when the model refused; the batch re-queues.
bool discard_refused_answer(std::map<int, std::string>& translations,
                            const std::string& stop_reason);

// Console preview length; full text goes to the side log file.
constexpr std::size_t RAW_ANSWER_PREVIEW = 2000;

// Path to the raw-answer side log. Empty disables it.
void set_raw_answer_log(const std::string& path);
const std::string& raw_answer_log();

// Side-file path beside a pipeline file.
std::string raw_answer_log_path(const std::string& sibling_file);

// One API round trip.  batch_lines: (speaker, jp).  previous_context:
// (speaker, en).  Throws anthropic::RateLimitError / ApiError.
std::map<int, std::string> call_anthropic(
    anthropic::Client& client,
    const std::vector<std::pair<std::string, std::string>>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context);

struct TranslateOptions {
    int batch_size = 150;
    bool retranslate = false;
    std::optional<std::string> only_script;
    bool test_mode = false;   // cap the run at num_batches API requests
    int num_batches = 1;
    bool fresh_run = false;
    bool include_untranslated = false;  // keep rows whose English is still Japanese
    std::string input_file;   // extracted_text.json
    std::string cache_file;   // translation_cache_anthropic.json
    std::string output_file;  // translated_text.json
    std::string tsv_file;     // translations.tsv, the table the game loads
};

// Run the full translation step. Returns 0 on success, 2 if the speaker gate fails.
int translate_all(const TranslateOptions& opt);

}  // namespace snu::translate
