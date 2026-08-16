// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Batch translation driver -- the whole of pipeline step 2.
//
// Translates scripts in STORY ORDER with speaker context.  Unlike a per-line
// speaker field, CatSystem2 encodes the speaker as a stateful NAME / MESSAGE /
// `fw 0` sequence, so `extract_message_runs` walks each script as a small
// state machine and yields (line_idx, speaker, content) triples.
//
// `translate_all` carries the speaker gate and the runtime-table build with it:
// everything that must happen for the game to show English happens inside the
// one command that pays for it.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/json.hpp>

#include "anthropic_client.h"

namespace ama::translate {

// Insertion-ordered jp -> en cache.  The order is load-bearing: it is the key
// order written to translation_cache_anthropic.json.
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

// One yielded message run: script line index, resolved speaker, raw content.
struct MessageRun {
    int line_idx = 0;
    std::string speaker;
    std::string content;
};

// Translatable = contains kana/CJK and isn't a `$var` placeholder.
bool needs_translation(const std::string& text);

// NAME-line content -> [Speaker] tag ('NARRATION' for `$var` / empty).
std::string get_speaker_tag(const std::string& name_content);

// Walk one script's lines as the NAME / MESSAGE / `fw 0` state machine.
std::vector<MessageRun> extract_message_runs(const boost::json::object& script);

// Remove [SPEAKER] tags Claude sometimes echoes despite the prompt.
std::string strip_speaker_tags(const std::string& text);

// Trim whitespace from both ends, U+3000 IDEOGRAPHIC SPACE included.
std::string strip(const std::string& s);

// De-double the quotes Claude adds when 「」 dialogue spans a \@ page break.
std::string fix_split_dialogue(const std::string& original, const std::string& translated);

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

// Record one translated line: `results` maps a line index to the English the
// run produced, and is what the run log is written from.  An empty `en` leaves
// the line out of both the cache and `results`, so the next run asks for it
// again and no log shows the Japanese as its own translation; the context
// window still gets the Japanese so the next request reads in sequence.
// `jp` is the line as extracted, `suffix` the trailing page break split off
// before the request and re-attached here.
void apply_line_result(Cache& cache, std::map<int, std::string>& results,
                       std::vector<std::pair<std::string, std::string>>& previous_context,
                       int idx, const std::string& speaker, const std::string& jp,
                       const std::string& suffix, const std::string& en);

// The user prompt sent per batch -- exposed for the speaker gate and tests.
std::string build_user_prompt(
    const std::vector<std::pair<std::string, std::string>>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context);

// Parse Claude's numbered response into {line_no -> translation}.
std::map<int, std::string> parse_numbered_response(const std::string& response_text);

// One API round trip.  batch_lines: (speaker, jp).  previous_context:
// (speaker, en).  Throws anthropic::RateLimitError / ApiError.
std::map<int, std::string> call_anthropic(
    anthropic::Client& client,
    const std::vector<std::pair<std::string, std::string>>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context);

// Reject option combinations that would spend money unintentionally, before
// anything is loaded or deleted.  Returns the reason, or nullopt when the
// options are usable.  A non-positive batch size is the dangerous case: it
// makes every request carry no lines, so a full run burns one request per
// script and answers nothing.
std::optional<std::string> validate_options(int batch_size, int test_n);

struct TranslateOptions {
    int batch_size = 150;
    bool retranslate = false;
    std::optional<std::string> only_script;
    bool test_mode = false;  // cap the run at num_batches API requests
    int num_batches = 1;
    // Start from nothing: drop the cache and the per-line document first, so
    // the table this run produces holds exactly what this run answered.
    bool fresh_run = false;
    std::string input_file;   // extracted_text.json
    std::string cache_file;   // translation_cache_anthropic.json
    std::string output_file;  // translated_text.json
    std::string last_translate_dir;
    std::string tsv_file;     // translation_table.tsv, the table the game loads
};

// The whole translation step, in the order the outputs depend on each other:
// the speaker gate, the batched translation, translated_text.json, and the
// runtime table built from it.  Returns a process exit code -- 2 when the gate
// refused, which happens before an API key is read and before any file is
// touched, so a refused run costs nothing and leaves nothing behind.
int translate_all(const TranslateOptions& opt);

}  // namespace ama::translate
