// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Batch translation driver and cache management for WelcomeToAnzU_CE.
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/json.hpp>

#include "cache/gemini_client.h"

namespace exm::translate {

constexpr int kDefaultBatchSize = 150;
inline constexpr std::size_t CACHE_DISCARD_THRESHOLD = 100;

// Insertion-ordered jp -> en cache hiding private data via PIMPL.
class Cache {
public:
    Cache();
    ~Cache();

    Cache(const Cache& other);
    Cache& operator=(const Cache& other);
    Cache(Cache&& other) noexcept;
    Cache& operator=(Cache&& other) noexcept;

    bool contains(const std::string& jp) const;
    const std::string* get(const std::string& jp) const;
    void set(const std::string& jp, const std::string& en);
    std::size_t size() const;
    const std::vector<std::pair<std::string, std::string>>& items() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

std::optional<std::string> validate_options(int batch_size, int test_n, int max_batches);

bool has_real_japanese(const std::string& text);
bool needs_translation(const std::string& text);
std::vector<std::size_t> translatable_indices(const boost::json::object& fdata);

std::string extract_speaker(const std::string& text);
std::string sanitize_ascii(const std::string& text);
std::string postprocess(const std::string& original, const std::string& translated);
std::string strip(const std::string& s);

Cache load_cache(const std::string& cache_file);
void save_cache(const Cache& cache, const std::string& cache_file);
std::size_t cache_entry_count(const std::string& cache_file);

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
    gemini::Client& client,
    const std::vector<std::pair<std::string, std::string>>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context,
    const std::vector<std::pair<std::string, std::string>>& context_after = {});

struct TranslateOptions {
    int batch_size = kDefaultBatchSize;
    bool retranslate = false;
    std::optional<std::string> only_file;
    int test_batches = 0;
    int max_batches = 0;
    std::string input_file;
    std::string cache_file;
    std::string output_file;
    std::string test_dir;
    std::string tsv_file;
};

int translate_all(const TranslateOptions& opt);

}  // namespace exm::translate
