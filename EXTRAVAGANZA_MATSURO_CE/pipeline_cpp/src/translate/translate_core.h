// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Batch translation driver (pipeline step 2).
//
// Translates each .spt file's strings in chronological order with a rolling
// context window.  There is no speaker field in this format: the speaker is
// derived from the text itself, because BLACKCyc stores dialogue as
// "SpeakerName\r\nDialogue" and the engine renders the first \r\n-segment as
// the name plate.
//
// The step owns everything the game needs to show English: the speaker gate
// runs first, the batched translation next, and the runtime table is built
// from the result last.  See translate_all at the bottom.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/json.hpp>

#include "anthropic_client.h"

namespace exm::translate {

// Insertion-ordered jp -> en cache.  Insertion order is load-bearing: it is
// the key order of translation_cache_anthropic.json, so a re-run that changes
// nothing rewrites an identical file.
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
// options are usable.  A negative batch cap is the dangerous case: it reads as
// "no limit" and starts an unbounded run.
std::optional<std::string> validate_options(int batch_size, int test_n, int max_batches);

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

// Unicode-aware strip (U+3000 included -- see util.h).
std::string strip(const std::string& s);

Cache load_cache(const std::string& cache_file);
void save_cache(const Cache& cache, const std::string& cache_file);

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
    int batch_size = 150;
    bool retranslate = false;
    std::optional<std::string> only_file;
    // --test N: cap the run at N API requests AND write a per-run smoke log
    // into test_dir.  The cache and the outputs are kept either way -- a
    // capped run must never cost work already paid for.
    int test_batches = 0;
    // --max-batches N: the same cap without the smoke log (0 = no limit).
    int max_batches = 0;
    std::string input_file;   // extracted_text.json
    std::string cache_file;   // translation_cache_anthropic.json
    std::string output_file;  // translated_text.json
    std::string test_dir;     // <project>/test (per-run smoke logs)
    std::string tsv_file;     // translation_table.tsv, the table the game loads
};

// The whole translation step, in the order the outputs depend on each other:
// the speaker gate, the batched translation, translated_text.json, and the
// runtime table built from it.  Returns a process exit code -- 2 when the gate
// refused, which happens before an API key is read and before any file is
// touched, so a refused run costs nothing and leaves nothing behind.
int translate_all(const TranslateOptions& opt);

}  // namespace exm::translate
