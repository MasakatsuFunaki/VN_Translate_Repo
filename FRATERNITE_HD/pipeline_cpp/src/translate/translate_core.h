// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Batch translation driver, and the whole of step 2: translate_all runs the
// speaker gate, translates, writes translated_text.json, and ends by building
// the runtime translation table.
//
// YU-RIS inlines the speaker into the line itself as `Name「...」` -- there is
// no separate name field and no \r\n split -- so the speaker is probed on
// EVERY entry regardless of the extractor's type tag.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/json.hpp>

#include "anthropic_client.h"

namespace frat::translate {

// jp -> en cache that keeps insertion order: that order is what gets written
// to translation_cache_anthropic.json, so a rerun must not reshuffle it.
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

// Reject option combinations that would spend money unintentionally, before
// anything is loaded or deleted.  Returns the reason, or nullopt when the
// options are usable.  A negative batch cap is the dangerous case: it reads
// as "no limit" and would start an unbounded run.
std::optional<std::string> validate_options(int batch_size, int test_n, int max_batches);

// Hiragana / katakana / CJK unified + ext-A.
bool has_real_japanese(const std::string& text);

// Non-blank, and either longer than 2 codepoints or containing real Japanese.
bool needs_translation(const std::string& text);

// Indices into a file's `strings` array worth translating.  "name" IS included
// here (the extractor tags one-word speaker plates that way).
std::vector<std::size_t> translatable_indices(const boost::json::object& fdata);

// The inline speaker plate, i.e.
//   ^([^　-〿「『（()0-9a-zA-Z\s]{1,12})[「『]
// hand-rolled: a narrow-string regex engine would match the negated Unicode
// class byte-wise and split kanji apart, so this walks codepoints instead
// (hazard R-regex-unicode).
std::optional<std::string> inline_speaker_match(const std::string& text);

// English speaker name for the rolling context, else "NARRATION".
std::string extract_speaker(const std::string& text);

// Replace the non-ASCII characters the LLM tends to emit with ASCII.
std::string sanitize_ascii(const std::string& text);

// True when the English still contains Japanese LETTERS.  Sound marks
// (U+309B/U+309C/U+30FC) are deliberately excluded so onomatopoeia survives.
bool en_has_japanese_content(const std::string& en);

// Romanise a leading JP speaker plate, re-attach dropped 「」, restore CRLF.
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

// STRICT parse of Claude's numbered response.  Only a clean [1..N] or
// [0..N-1] sequence is accepted; a dropped, extra, gapped or duplicated
// number returns {} so the batch retries next run instead of writing
// misaligned (JP, EN) pairs into the cache.
std::map<int, std::string> parse_translations(const std::string& response_text,
                                              std::size_t expected_count);

// One API round trip.  Throws anthropic::RateLimitError / ApiError.
std::map<int, std::string> call_anthropic(
    anthropic::Client& client,
    const std::vector<std::pair<std::string, std::string>>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context);

struct TranslateOptions {
    int batch_size = 150;
    bool retranslate = false;
    std::optional<std::string> only_file;
    // Cap on API requests; 0 means no limit.  `--test N` and `--max-batches N`
    // both set it and differ only in what they do to the files on disk.
    int test_batches = 0;
    // A smoke run: it writes the per-run answer log under test_dir.
    bool test_mode = false;
    // Delete the cache and the per-line document first, so the table this run
    // produces holds exactly what this run answered.
    bool fresh_run = false;
    std::string input_file;   // extracted_text.json
    std::string cache_file;   // translation_cache_anthropic.json
    std::string output_file;  // translated_text.json
    std::string tsv_file;     // translation_table.tsv, the table the game loads
    std::string test_dir;     // <project>/test (per-run smoke logs)
};

// The whole translation step: the speaker gate, the batched translation, the
// translated document, and the runtime table.  Returns the process exit code
// (2 when the gate fails, and then nothing has been written).
int translate_all(const TranslateOptions& opt);

}  // namespace frat::translate
