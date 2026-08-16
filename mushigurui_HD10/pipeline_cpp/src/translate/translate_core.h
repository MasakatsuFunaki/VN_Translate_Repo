// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Batch translation driver.
//
// Translates each .spt file's strings in chronological order with a rolling
// context window.  There is no speaker field in this engine: the speaker is
// derived from the text itself, since BLACKCyc dialogue is stored as
// "SpeakerName\r\nDialogue" and the engine renders the first \r\n-segment as
// the name plate.
//
// One call does the whole step.  The speaker gate runs first, before a key is
// read; the runtime table is built last, from the document just written.  A
// translation the game never sees is not a translation, so producing it is
// not a separate command someone has to remember.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/json.hpp>

#include "anthropic_client.h"

namespace mgi::translate {

// Insertion-ordered jp -> en cache.  Insertion order is load-bearing: it is
// the key order of the cache file on disk, which makes a resumed run's diff
// against the previous run readable.
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

// Real Japanese only -- kana + CJK.  Deliberately NARROWER than
// spt::has_japanese(), which also accepts fullwidth forms and CJK symbols:
// a string of nothing but fullwidth punctuation is not worth an API call.
bool has_real_japanese(const std::string& text);

// Translatable = non-blank, has real Japanese, and not a <=3-char non-JP run.
bool needs_translation(const std::string& text);

// Indices into a file's `strings` array that are worth translating.
std::vector<std::size_t> translatable_indices(const boost::json::object& fdata);

// Speaker plate from "Name\r\n「Dialogue」", else "NARRATION".
std::string extract_speaker(const std::string& text);

// Replace the non-ASCII characters the LLM tends to emit with ASCII.
std::string sanitize_ascii(const std::string& text);

// Re-attach the English speaker plate and restore a trailing CRLF.
std::string postprocess(const std::string& original, const std::string& translated);

// Trim whitespace over the full Unicode set, U+3000 included (see mgi::trim).
std::string strip(const std::string& s);

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

// The user prompt sent per batch -- exposed for the gate and tests.
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

// Reject a command line that cannot mean anything before it costs anything.
// Returns the reason when the pair is unusable, nullopt when it is fine.
std::optional<std::string> validate_options(int batch_size, int test_batches);

struct TranslateOptions {
    int batch_size = 150;
    bool retranslate = false;
    std::optional<std::string> only_file;
    int test_batches = 0;
    std::string input_file;   // extracted_text.json
    std::string cache_file;   // translation_cache_anthropic.json
    std::string output_file;  // translated_text.json
    std::string tsv_file;     // translation_table.tsv, the table the game loads
    std::string test_dir;     // <project>/test (per-run smoke logs)
};

// The whole translation step: the speaker gate, the batched translation, the
// translated document, and the runtime table the game reads.  Returns the
// process exit code.
int translate_all(const TranslateOptions& opt);

}  // namespace mgi::translate
