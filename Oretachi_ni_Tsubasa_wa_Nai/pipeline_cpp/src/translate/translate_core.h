// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "translate/gemini_client.h"

namespace oretsuba::translate {

constexpr int kExitOk = 0;
constexpr int kExitError = 1;
constexpr int kExitGateOrRefusal = 2;

struct TranslationCache {
    std::vector<std::string> order;
    std::unordered_map<std::string, std::string> values;

    const std::string* Find(std::string_view key) const;
    void Set(std::string key, std::string value);
    bool Erase(std::string_view key);
    bool Contains(std::string_view key) const;
    std::size_t Size() const noexcept { return order.size(); }
    bool Empty() const noexcept { return order.empty(); }
};

struct TranslateOptions {
    int batch_size = kDefaultBatchSize;
    int max_batches = 0;
    bool test_mode = false;
    int test_batches = 1;
    bool retranslate = false;
    bool discard_cache = false;
    bool rebuild_table_only = false;
    std::string extracted_file;
    std::string translated_file;
    std::string cache_file;
    std::string tsv_file;
    std::string last_batch_log_file;
    std::string tone_file;
    std::string project_dir;
};

// Thread-compatible orchestrator for context-aware batch translation,
// atomic cache checkpointing, and TSV table generation.
class TranslateCore {
public:
    TranslateCore();
    explicit TranslateCore(ITranslationBackend* backend_override);
    TranslateCore(const TranslateCore&) = delete;
    TranslateCore& operator=(const TranslateCore&) = delete;
    TranslateCore(TranslateCore&& other) noexcept;
    TranslateCore& operator=(TranslateCore&& other) noexcept;
    ~TranslateCore();

    int Run(const TranslateOptions& options);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

bool NeedsTranslation(std::string_view text);

TranslationCache LoadCache(const std::string& cache_file);
void SaveCache(const std::string& cache_file, const TranslationCache& cache);
std::size_t CacheEntryCount(const std::string& cache_file);

bool RefuseCacheDiscard(const TranslateOptions& options,
                        std::size_t existing_entries,
                        std::string* reason_out = nullptr);

bool IsFailedEntry(std::string_view key, std::string_view val);
std::size_t PurgeFailedEntries(TranslationCache& cache);

bool ApplyLineResult(std::string_view jp_text,
                     const std::map<int, std::string>& batch_results,
                     int one_based_index,
                     TranslationCache& cache);

void ValidateOptions(const TranslateOptions& options);

int TranslateAll(const TranslateOptions& options,
                 ITranslationBackend* backend_override = nullptr);

}  // namespace oretsuba::translate
