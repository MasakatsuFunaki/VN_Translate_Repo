// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Step 2: speaker gate, batched translation, translated_text.json, and TSV build.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/json.hpp>

#include "anthropic_client.h"

namespace frat::translate {

// Insertion-ordered jp->en cache; order is the on-disk key order.
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

// Returns the reason the options are invalid, or nullopt.
std::optional<std::string> validate_options(int batch_size, int test_n, int max_batches);

bool has_real_japanese(const std::string& text);
bool needs_translation(const std::string& text);
std::vector<std::size_t> translatable_indices(const boost::json::object& fdata);

// Hand-rolled — a narrow-string regex would split kanji byte-wise.
std::optional<std::string> inline_speaker_match(const std::string& text);

std::string extract_speaker(const std::string& text);
std::string sanitize_ascii(const std::string& text);

// Sound marks (U+309B/U+309C/U+30FC) excluded so onomatopoeia survives.
bool en_has_japanese_content(const std::string& en);

std::string postprocess(const std::string& original, const std::string& translated);

Cache load_cache(const std::string& cache_file);
void save_cache(const Cache& cache, const std::string& cache_file);
std::size_t cache_entry_count(const std::string& cache_file);

inline constexpr std::size_t CACHE_DISCARD_THRESHOLD = 100;

// Returns refusal reason, or nullopt when deletion is allowed.
std::optional<std::string> refuse_cache_discard(std::size_t cache_entries,
                                                const std::string& flag,
                                                bool discard_cache);

bool is_failed_entry(const std::string& jp, const std::string& en);
std::size_t purge_failed_entries(Cache& cache);

std::string build_user_prompt(
    const std::vector<std::pair<std::string, std::string>>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context);

// Only a clean [1..N] or [0..N-1] sequence is accepted; {} on mismatch.
std::map<int, std::string> parse_translations(const std::string& response_text,
                                              std::size_t expected_count);

std::map<int, std::string> call_anthropic(
    anthropic::Client& client,
    const std::vector<std::pair<std::string, std::string>>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context);

struct TranslateOptions {
    int batch_size = 150;
    bool retranslate = false;
    std::optional<std::string> only_file;
    int test_batches = 0;
    bool test_mode = false;
    bool fresh_run = false;
    std::string input_file;
    std::string cache_file;
    std::string output_file;
    std::string tsv_file;
    std::string test_dir;
};

// Returns 0 on success, 2 when the gate fails (nothing written).
int translate_all(const TranslateOptions& opt);

}  // namespace frat::translate
