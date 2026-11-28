// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Batched Gemini script translation, cache persistence, and runtime TSV generation.
//
// Scenes are walked in chronological `story_order()` with a 50-line rolling
// context window, forward uncached unique-line batch packing, global
// `max_batches` enforcement across all scenes, and live progress snapshots at
// `script_output/last_gemini_translate/last_translation.json`.
#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/json.hpp>

#include "translate/gemini_client.h"
#include "vn/llm/llm_types.h"

namespace adi::translate {

// Insertion-ordered jp -> en cache. Insertion order decides the key order of
// the cache file, so updating an existing key preserves its original slot.
class Cache {
public:
    bool contains(const std::string& jp) const { return index_.count(jp) != 0; }
    const std::string* get(const std::string& jp) const {
        auto it = index_.find(jp);
        return it == index_.end() ? nullptr : &order_[it->second].second;
    }
    void set(const std::string& jp, const std::string& en) {
        auto it = index_.find(jp);
        if (it != index_.end()) {
            order_[it->second].second = en;
            return;
        }
        index_.emplace(jp, order_.size());
        order_.emplace_back(jp, en);
    }
    std::size_t size() const { return order_.size(); }
    const std::vector<std::pair<std::string, std::string>>& items() const { return order_; }

private:
    std::vector<std::pair<std::string, std::string>> order_;
    std::map<std::string, std::size_t> index_;
};

std::optional<std::string> validate_options(int batch_size, int test_n, int max_batches);

bool has_real_japanese(const std::string& text);

bool needs_translation(const std::string& text);

std::vector<std::size_t> translatable_indices(const boost::json::object& scene_data,
                                              bool skip_to_dialogue);

std::optional<std::string> match_speaker_name(const std::string& jp_name);

std::string extract_speaker(const boost::json::object& s);

std::string sanitize_ascii(const std::string& text);

std::string postprocess(const std::string& original, const std::string& translated);

std::string flatten_for_log(const std::string& txt, std::size_t limit);

Cache load_cache(const std::string& cache_file);
void save_cache(const Cache& cache, const std::string& cache_file);

// Seeds `cache` with `name_translations_ordered()` and `default_seed_translations()`
// for any key not already present.
void seed_default_glossary(Cache& cache);

std::size_t cache_entry_count(const std::string& cache_file);

inline constexpr std::size_t CACHE_DISCARD_THRESHOLD = 100;

std::optional<std::string> refuse_cache_discard(std::size_t cache_entries,
                                                const std::string& flag,
                                                bool discard_cache);

bool is_failed_entry(const std::string& jp, const std::string& en);

std::size_t purge_failed_entries(Cache& cache);

std::string build_user_prompt(
    const std::vector<std::pair<std::string, std::string>>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context,
    const std::vector<std::pair<std::string, std::string>>& context_after = {});

std::map<int, std::string> parse_numbered_response(const std::string& response_text);

std::map<int, std::string> call_gemini(
    gemini::IMessageClient& client,
    const std::vector<std::pair<std::string, std::string>>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context,
    const std::vector<std::pair<std::string, std::string>>& context_after = {});

struct TranslateOptions {
    int batch_size = vn::llm::kDefaultBatchSize;
    bool retranslate = false;
    std::optional<std::string> only_scene;
    int max_batches = 0; // Global cap on API requests across all scenes (0 = unlimited)
    bool fresh_run = false;
    bool skip_to_dialogue = false;
    std::string input_file;   // extracted_text.json
    std::string cache_file;   // translation_cache_gemini.json
    std::string output_file;  // translated_text.json
    std::string tsv_file;     // translation_table.tsv
    std::string test_dir;     // <project>/test (optional smoke logs)
    std::function<void(std::chrono::milliseconds)> sleep_fn;
};

// Translates a single scene, packing up to `opt.batch_size` uncached unique
// lines per API request while advancing `api_batches_done` globally. Returns
// true if `opt.max_batches > 0` halted remaining uncached work in this scene.
bool translate_scene(const std::string& scene_name,
                     const boost::json::object& scene_data,
                     Cache& cache,
                     const TranslateOptions& opt,
                     gemini::IMessageClient& client,
                     int& api_batches_done);

int translate_all(const TranslateOptions& opt);
int translate_all(const TranslateOptions& opt, gemini::IMessageClient& client);

}  // namespace adi::translate
