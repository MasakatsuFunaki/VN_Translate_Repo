// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Batch translation driver.
//
// CROSS_CHANNEL is "Pattern A": 01_extract writes an explicit `speaker` field
// per entry, so extract_speaker reads the JSON object rather than parsing the
// text.  Getting that wrong once made every dialogue line report NARRATION and
// cost a whole translation run -- the speaker gate in speaker_gate.h runs
// first inside translate_all to catch that regression before any token is
// spent.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/json.hpp>

#include "anthropic_client.h"

namespace crc::translate {

// Insertion-ordered jp -> en cache.  Insertion order is load-bearing: it is
// the key order of translation_cache_anthropic.json, so a resumed run appends
// instead of reshuffling the whole file.  Never a sorted or hashed container.
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

// Kana + CJK only.  Deliberately NARROWER than extract::is_real_japanese,
// which also accepts the 3000-303F symbol block: a line of nothing but
// fullwidth punctuation is not worth an API call.
bool has_real_japanese(const std::string& text);

bool needs_translation(const std::string& text);

// Indices into a file's `strings` array worth translating (dialogue +
// narration; CROSS_CHANNEL has no other entry types).
std::vector<std::size_t> translatable_indices(const boost::json::object& fdata);

// Speaker for one entry object: the `speaker` field, falling back to the text
// before the first \r\n for legacy entries.  Returns the English name when the
// glossary knows it, the raw JP name otherwise, "NARRATION" when there is none.
std::string extract_speaker(const boost::json::object& s);

// Replace the non-ASCII characters the LLM tends to emit with ASCII.
std::string sanitize_ascii(const std::string& text);

// Re-attach the English speaker plate and restore a trailing CRLF.
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

struct TranslateOptions {
    int test_batches = 0;
    std::string input_file;   // extracted_text.json
    std::string cache_file;   // translation_cache_anthropic.json
    std::string output_file;  // translated_text.json
    std::string test_dir;     // <project>/test (per-run smoke logs)
    std::string tsv_file;     // translations.tsv, built from output_file
    std::string game_tsv_file;  // where the table is copied for the game
};

// The whole translation step: the speaker gate, the batched translation, the
// translated document, and the runtime table the game loads.  Returns a
// process exit code -- 2 when the gate refuses, before anything is spent.
int translate_all(const TranslateOptions& opt);

}  // namespace crc::translate
