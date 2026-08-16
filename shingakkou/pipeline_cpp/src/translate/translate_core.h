// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Batch translation driver.
//
// Files are translated in STORY_ORDER with a 50-line rolling context window.
// Speakers come from the JSON `speaker` FIELD that 01_extract populated -- not
// from parsing the line text, which is what the other BLACKCyc-style pipelines
// do.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/json.hpp>

#include "anthropic_client.h"

namespace shin::translate {

// Insertion-ordered jp -> en cache.  Insertion order is load-bearing: it
// decides the cache file's key order, and re-setting an existing key must NOT
// move it -- that is what lets the 75 glossary names be re-seeded at the top of
// every run without reordering the other 40k entries.
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
    std::size_t size() const { return order_.size(); }
    const std::vector<std::pair<std::string, std::string>>& items() const { return order_; }

private:
    std::vector<std::pair<std::string, std::string>> order_;
    std::map<std::string, std::size_t> index_;
};

// Kana + CJK + CJK Ext-A.  NARROWER than extract::has_japanese, which accepts
// fullwidth forms instead of Ext-A -- the two predicates are not the same
// function and swapping them changes the translatable set.
bool has_real_japanese(const std::string& text);

// Sprite / expression control codes, which are never player-visible.
bool is_control_code(const std::string& text);

// Blank, control-code and short-non-Japanese strings are not worth an API call.
bool needs_translation(const std::string& text);

// Indices into a file's `strings` array worth translating.
std::vector<std::size_t> translatable_indices(const boost::json::object& fdata);

// English speaker plate, read from the record's `speaker` FIELD.
std::string extract_speaker(const boost::json::object& s);

// Replace the non-ASCII characters the LLM tends to emit with ASCII.
std::string sanitize_ascii(const std::string& text);

// Sanitise only.  Unlike the BLACKCyc pipelines this does NOT re-attach a
// speaker plate: step 3 prepends one itself, so doing it here would put the
// name in twice ("Michael\nMichael\nHello" at runtime).
std::string postprocess(const std::string& original, const std::string& translated);

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

// The user prompt sent per batch -- exposed for the tests.
std::string build_user_prompt(
    const std::vector<std::pair<std::string, std::string>>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context);

// Parse Claude's numbered response into {line_no -> translation}.
std::map<int, std::string> parse_numbered_response(const std::string& response_text);

// One API round trip.  Throws anthropic::RateLimitError / ApiError.
std::map<int, std::string> call_anthropic(
    anthropic::Client& client,
    const std::vector<std::pair<std::string, std::string>>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context);

struct TranslateOptions {
    int batch_size = 150;
    bool retranslate = false;
    std::optional<std::string> only_file;
    int test_batches = 0;
    bool fresh_run = false;   // drop the cache and the document before starting
    std::string input_file;   // extracted_text.json
    std::string cache_file;   // translation_cache_anthropic.json
    std::string output_file;  // translated_text.json
    std::string tsv_file;     // translation_table.tsv, in the game folder
    std::string test_dir;     // <project>\test (per-run smoke logs)
};

// Reject a command line that cannot mean anything, before a key is read or a
// file is touched.  Returns the reason, or nullopt when the options are sane.
std::optional<std::string> validate_options(int batch_size, int test_n);

// The whole translation step: the speaker gate, the batched translation, the
// translated document, and the runtime table the proxy DLL loads.  Returns the
// process exit code -- 2 when the gate refused, and then nothing was written.
int translate_all(const TranslateOptions& opt);

}  // namespace shin::translate
