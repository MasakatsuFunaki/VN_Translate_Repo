// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// The whole translation step: the cast-roster gate, the batched translation,
// translated_text.json, and the runtime table the game loads.
//
// Translates Sakura.hcb strings in stream order (the order they appear in the
// bytecode, which is roughly story order), with a rolling context window of
// previous translations for continuity.  Resume-safe cache keyed by JP
// content.  Materialises translated_text.json where every line gains a
// 'translated' field, then builds translation_table.tsv from it -- a
// translation the game never sees is not a translation, so the table is not a
// second command someone has to remember.
//
// Unlike the other games in this repo there are no [Speaker] tags: the FVP engine
// does not tag per-line speakers, so Gemini infers them from prose context
// plus the <character_dossier> in the system prompt.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/json.hpp>

#include "gemini_client.h"

namespace fav::translate {

// Insertion-ordered jp -> en cache.  The order is load-bearing: it decides the
// key order of translation_cache_gemini.json, which is diffed run-to-run.
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

// Remove [Speaker] tags Gemini sometimes echoes despite the prompt.
std::string strip_speaker_tags(const std::string& text);

// Replace typographic punctuation with CP932-safe ASCII and strip leftover
// [ruby|base] bracket markup.
std::string sanitize_ascii(const std::string& text);

// Unicode-aware whitespace strip -- U+3000 included (see common/util.h).
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

// Cache one translated line. Empty `en` leaves it uncached so it re-queues,
// and the Japanese carries the context window instead.
void apply_line_result(Cache& cache, std::vector<std::string>& previous_context,
                       const std::string& jp, const std::string& en);

// The user prompt sent per batch -- exposed for the gate/tests.
std::string build_user_prompt(const std::vector<std::string>& batch_lines,
                              const std::vector<std::string>& previous_context,
                              const std::vector<std::string>& context_after = {});

// Parse Gemini's numbered response into {line_no -> translation}.
std::map<int, std::string> parse_numbered_response(const std::string& response_text);

// One API round trip.  batch_lines: JP content (optionally [Speaker]-prefixed).
// previous_context: prior EN.  context_after: upcoming JP lookahead.
// Throws gemini::RateLimitError / ApiError.
std::map<int, std::string> call_gemini(
    gemini::Client& client, const std::vector<std::string>& batch_lines,
    const std::vector<std::string>& previous_context,
    const std::vector<std::string>& context_after = {});

// Reject option combinations that would spend money unintentionally, before
// anything is loaded or deleted.  Returns the reason, or nullopt when the
// options are usable.  A negative batch cap is the dangerous case: it reads as
// "no limit" and starts an unbounded run.
std::optional<std::string> validate_options(int batch_size, int test_n, int max_batches);

struct TranslateOptions {
    int batch_size = 100;
    bool retranslate = false;
    bool test_mode = false;   // cap the run at num_batches API requests
    bool fresh_run = false;   // drop the cache and the document before starting
    int num_batches = 1;
    std::string hcb_file;        // <game-dir>\Sakura.hcb, the gate's input
    std::string input_file;      // extracted_text.json
    std::string cache_file;      // translation_cache_gemini.json
    std::string output_file;     // translated_text.json
    std::string tsv_file;        // <game-dir>\translation_table.tsv
    std::string last_translate_dir;  // script_output/last_gemini_translate
};

// The whole translation step: gate, translate, write the document, build the
// table.  Returns a process exit code -- 2 when the gate refuses, in which
// case nothing was spent and nothing was written.
int translate_all(const TranslateOptions& opt);

}  // namespace fav::translate
