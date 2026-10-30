// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Batch translation driver.
//
// Reads extracted_text.json, translates in batches with a rolling
// per-script context window, caches every successful API call
// (resume-safe), and materialises translated_text.json where every entry
// gains a 'translated' field.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/json.hpp>

#include "gemini_client.h"

namespace snt::translate {

// jp -> en cache.  Insertion order is load-bearing: it decides the key order
// of translation_cache_gemini.json, so a resumed run appends to the file
// instead of rewriting it in a different order.
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

// Remove a [Speaker] tag the model echoed at the front of its answer.  A
// bracketed group further along the line is content and stays.
std::string strip_speaker_tags(const std::string& text);

// max_tokens for a batch, clamped to the model's output ceiling.
std::int64_t compute_max_tokens(int batch_size);

// Trim leading/trailing whitespace, U+3000 IDEOGRAPHIC SPACE included -- the
// model pads its replies with it after Japanese input.
std::string strip(const std::string& s);

// Turn a literal "\n" the answer wrote into a real break, and drop the ASCII
// blanks written against it.  U+3000 beside the escape is kept: it is the
// paragraph indent the prompt asks the answer to copy, not a blank.
//
// There is no escaping counterpart on the prompt side.  No Japanese string in
// this corpus carries a line break or a backslash (0 of 92,774 entries), so
// the only breaks that can exist are the ones the English introduces.
std::string unescape_line_breaks(const std::string& s);

// ── Speaker resolution ──
// Sakura no Toki .ast files emit a `name = {"<speaker>"}` block immediately
// before each dialogue line, and step 1 records the plate that was standing
// as a `speaker` field on every dialogue and narrative entry.  That field is
// what a tag is built from.
//
// The state machine below is the fallback for a document written before the
// field existed: walk the strings in order tracking the most recent name.  It
// cannot see what the extractor saw -- a ruby base split out of the middle of
// a spoken line reads as narration to it -- so the field wins wherever both
// have an answer.
//
// An empty `current_speaker` means no name has been seen yet -- the entry is
// tagged [NARRATION] rather than attributed to the wrong character.

// [Speaker] tag for an entry from the state machine alone, given the running
// state.  Ignores the `speaker` field.
std::string extract_speaker(const boost::json::object& entry,
                            const std::string& current_speaker);

// State transition after processing an entry.
std::string update_speaker(const boost::json::object& entry,
                           const std::string& current_speaker);

// Where the tags of one script came from, for the caller to report.
struct SpeakerSource {
    std::size_t from_field = 0;     // entries whose tag came from the field
    std::size_t disagreements = 0;  // of those, tags the state machine differs on
    std::int64_t first_disagreement_offset = -1;  // -1 when there was none
};

// (speaker_tag, entry) for every entry in script order.  An entry carrying a
// `speaker` field is tagged from it, whether the field names a speaker or is
// null for narration; an entry without the field falls back to the state
// machine.  Pass `source` to learn which of the two answered.
std::vector<std::pair<std::string, const boost::json::object*>>
iter_entries_with_speakers(const boost::json::array& entries,
                           SpeakerSource* source = nullptr);

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
void apply_line_result(Cache& cache,
                       std::vector<std::pair<std::string, std::string>>& previous_context,
                       const std::string& speaker, const std::string& jp,
                       const std::string& en);

// The user prompt sent per batch -- exposed for the speaker gate.
std::string build_user_prompt(
    const std::vector<std::pair<std::string, std::string>>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context);

// Parse the model's "1. ... 2. ..." answer into {line_no -> translation}.
// A line number is read as one only when it is strictly greater than the last
// and, when `expected` is non-zero, no greater than `expected`; anything else
// is prose and joins the entry it follows.  Numbering that cannot be read at
// all empties the whole map, so the caller re-queues the batch instead of
// caching translations that may be one line out of step.
//
// `rejected_lines` receives how many lines were refused, and
// `final_entry_tail` the text that ran on after the last entry.  Both are
// optional.  `expected` = 0 disables the upper bound.
std::map<int, std::string> parse_numbered_response(const std::string& response_text,
                                                   int expected = 0,
                                                   int* rejected_lines = nullptr,
                                                   std::string* final_entry_tail = nullptr);

// True when the parsed count can be lined up with the batch: either it matches,
// or the answer was cut off, where a short count is expected and the fragment
// has already been dropped.
bool answer_count_is_usable(std::size_t requested, std::size_t parsed,
                            const std::string& stop_reason);

// Remove the last entry when the answer was cut mid-sentence.  Returns whether
// anything was removed.
bool drop_truncated_entry(std::map<int, std::string>& translations,
                          const std::string& stop_reason);

// Clear everything when the model refused, so the batch re-queues.  Returns
// whether the refusal applied.
bool discard_refused_answer(std::map<int, std::string>& translations,
                            const std::string& stop_reason);

// What one response says: the first text block, and why the model stopped.
// Thinking blocks come first with adaptive thinking, so the walk skips them.
struct Answer {
    std::string text;
    std::string stop_reason;  // empty when the response carries none
};

Answer read_answer(const boost::json::value& response);

// The translations one answer can be trusted to hold, after the guards above.
// An answer that fails them yields an empty map and the batch re-queues.
std::map<int, std::string> usable_translations(const Answer& answer, std::size_t requested);

// Console preview length; the whole answer goes to the side log.
constexpr std::size_t RAW_ANSWER_PREVIEW = 2000;

// Path of the raw-answer side log.  Empty disables it, which is the default.
void set_raw_answer_log(const std::string& path);
const std::string& raw_answer_log();

// The side-log path beside a pipeline file.
std::string raw_answer_log_path(const std::string& sibling_file);

// One API round trip.  batch_lines: (speaker, jp).  previous_context:
// (speaker, en).  Throws gemini::RateLimitError / ApiError.
std::map<int, std::string> call_gemini(
    gemini::Client& client,
    const std::vector<std::pair<std::string, std::string>>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context);

// How long a rate-limited batch waits before its one retry.
inline constexpr std::chrono::seconds RATE_LIMIT_WAIT{60};

// Asks for one batch and returns {line_no -> translation}; throws as
// call_gemini does.
using BatchAnswer = std::function<std::map<int, std::string>()>;

// Ask for one batch and write what came back into the cache.  Every failure
// path -- a throw, a rate limit whose retry also throws, a line the answer
// left out -- leaves that line uncached, so the next run asks for it again.
// The Japanese still reaches the context window, so the following request
// reads in sequence.
void translate_batch(Cache& cache,
                     std::vector<std::pair<std::string, std::string>>& previous_context,
                     const std::vector<std::pair<std::string, std::string>>& batch,
                     const BatchAnswer& ask,
                     std::chrono::seconds retry_wait = RATE_LIMIT_WAIT);

// Reject option combinations that would spend money unintentionally, before
// anything is loaded or deleted.  Returns the reason, or nullopt when the
// options are usable.  A negative batch cap is the dangerous case: it reads
// as "no limit" everywhere the cap is compared against zero, so a mistyped
// one would start an unbounded run.
std::optional<std::string> validate_options(int batch_size, int test_n, int max_batches);

struct TranslateOptions {
    int batch_size = 150;
    bool retranslate = false;
    std::optional<std::string> only_script;
    bool test_mode = false;  // cap the run at num_batches API requests
    int num_batches = 1;
    // Start from nothing: drop the cache and the per-line document first, so
    // the table this run produces holds exactly what this run answered.
    bool fresh_run = false;
    bool include_untranslated = false;  // keep rows whose English is still Japanese
    // Run the breaks inside a table value together instead of keeping them.
    bool flow_breaks = false;
    // Run the gate over the extraction and stop.  No API key is read, no
    // request is sent, and no file is written or removed.
    bool gate_only = false;
    std::string input_file;   // extracted_text.json
    std::string cache_file;   // translation_cache_gemini.json
    std::string output_file;  // translated_text.json
    std::string tsv_file;     // translations.tsv, the table the game loads
};

// The whole translation step, in the order the outputs depend on each other:
// the speaker gate, the batched translation, translated_text.json, and the
// runtime table built from it.  Returns a process exit code -- 2 when the gate
// refused, which happens before an API key is read and before any file is
// touched, so a refused run costs nothing and leaves nothing behind.
int translate_all(const TranslateOptions& opt);

}  // namespace snt::translate
