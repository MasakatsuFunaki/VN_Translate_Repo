// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include "translate/translate_core.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_set>

#include <boost/json.hpp>

#include "build_tsv/build_tsv.h"
#include "common/util.h"
#include "translate/glossary.h"
#include "translate/speaker_gate.h"

namespace oretsuba::translate {

namespace {

constexpr std::uint32_t kHiraganaStart = 0x3040U;
constexpr std::uint32_t kKatakanaEnd = 0x30FFU;
constexpr std::uint32_t kCjkUnifiedStart = 0x4E00U;
constexpr std::uint32_t kCjkUnifiedEnd = 0x9FFFU;
constexpr std::uint32_t kHalfwidthKatakanaStart = 0xFF66U;
constexpr std::uint32_t kHalfwidthKatakanaEnd = 0xFF9FU;
constexpr std::uint32_t kFullwidthDigitZero = 0xFF10U;
constexpr std::uint32_t kFullwidthDigitNine = 0xFF19U;

constexpr unsigned char kUtf8AsciiMax = 0x7FU;
constexpr unsigned char kUtf8TwoByteMask = 0xE0U;
constexpr unsigned char kUtf8TwoBytePrefix = 0xC0U;
constexpr unsigned char kUtf8ThreeByteMask = 0xF0U;
constexpr unsigned char kUtf8ThreeBytePrefix = 0xE0U;
constexpr unsigned char kUtf8FourByteMask = 0xF8U;
constexpr unsigned char kUtf8FourBytePrefix = 0xF0U;
constexpr unsigned char kUtf8PayloadMask = 0x3FU;
constexpr unsigned char kUtf8TwoByteLeadMask = 0x1FU;
constexpr unsigned char kUtf8ThreeByteLeadMask = 0x0FU;
constexpr unsigned char kUtf8FourByteLeadMask = 0x07U;

constexpr int kMaxApiRetries = 5;
constexpr int kInitialRetryBackoffSec = 3;
constexpr int kBackoffMultiplier = 2;

std::vector<std::uint32_t> DecodeCodepoints(std::string_view utf8) {
    std::vector<std::uint32_t> out;
    out.reserve(utf8.size());
    std::size_t i = 0;
    while (i < utf8.size()) {
        const auto c = static_cast<unsigned char>(utf8[i]);
        if (c <= kUtf8AsciiMax) {
            out.push_back(c);
            ++i;
            continue;
        }
        if ((c & kUtf8TwoByteMask) == kUtf8TwoBytePrefix && i + 1 < utf8.size()) {
            const std::uint32_t cp =
                ((static_cast<std::uint32_t>(c & kUtf8TwoByteLeadMask) << 6U) |
                 static_cast<std::uint32_t>(static_cast<unsigned char>(utf8[i + 1]) & kUtf8PayloadMask));
            out.push_back(cp);
            i += 2;
            continue;
        }
        if ((c & kUtf8ThreeByteMask) == kUtf8ThreeBytePrefix && i + 2 < utf8.size()) {
            const std::uint32_t cp =
                ((static_cast<std::uint32_t>(c & kUtf8ThreeByteLeadMask) << 12U) |
                 (static_cast<std::uint32_t>(static_cast<unsigned char>(utf8[i + 1]) & kUtf8PayloadMask) << 6U) |
                 static_cast<std::uint32_t>(static_cast<unsigned char>(utf8[i + 2]) & kUtf8PayloadMask));
            out.push_back(cp);
            i += 3;
            continue;
        }
        if ((c & kUtf8FourByteMask) == kUtf8FourBytePrefix && i + 3 < utf8.size()) {
            const std::uint32_t cp =
                ((static_cast<std::uint32_t>(c & kUtf8FourByteLeadMask) << 18U) |
                 (static_cast<std::uint32_t>(static_cast<unsigned char>(utf8[i + 1]) & kUtf8PayloadMask) << 12U) |
                 (static_cast<std::uint32_t>(static_cast<unsigned char>(utf8[i + 2]) & kUtf8PayloadMask) << 6U) |
                 static_cast<std::uint32_t>(static_cast<unsigned char>(utf8[i + 3]) & kUtf8PayloadMask));
            out.push_back(cp);
            i += 4;
            continue;
        }
        out.push_back(c);
        ++i;
    }
    return out;
}

bool IsJapaneseChar(std::uint32_t cp) {
    if (cp >= kHiraganaStart && cp <= kKatakanaEnd) {
        return true;
    }
    if (cp >= kCjkUnifiedStart && cp <= kCjkUnifiedEnd) {
        return true;
    }
    if (cp >= kHalfwidthKatakanaStart && cp <= kHalfwidthKatakanaEnd) {
        return true;
    }
    return false;
}

bool IsNumericOrWhitespace(std::uint32_t cp) {
    if (cp == ' ' || cp == '\t' || cp == '\r' || cp == '\n' || cp == 0x3000U) {
        return true;
    }
    if (cp >= '0' && cp <= '9') {
        return true;
    }
    if (cp >= kFullwidthDigitZero && cp <= kFullwidthDigitNine) {
        return true;
    }
    return false;
}

std::string ReadWholeFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void WriteFileAtomic(const std::filesystem::path& path, std::string_view content) {
    if (path.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    const std::filesystem::path tmp_path = path.string() + ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("Cannot open temporary file for writing: " + tmp_path.string());
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    std::error_code ec;
    std::filesystem::rename(tmp_path, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(tmp_path, path, ec);
        if (ec) {
            throw std::runtime_error("Failed to rename temporary file to " + path.string() +
                                     ": " + ec.message());
        }
    }
}

void WriteJsonObjectOrdered(
    std::ostream& out,
    const std::vector<std::string>& order,
    const std::unordered_map<std::string, std::string>& values) {
    out << "{\n";
    bool first = true;
    for (const auto& k : order) {
        const auto it = values.find(k);
        if (it == values.end()) {
            continue;
        }
        if (!first) {
            out << ",\n";
        }
        first = false;
        out << "  " << boost::json::serialize(boost::json::value(k)) << ": "
            << boost::json::serialize(boost::json::value(it->second));
    }
    out << "\n}\n";
}

void PrettyWriteJson(std::ostream& out, const boost::json::value& val, int indent = 0) {
    const std::string pad(static_cast<std::size_t>(indent * 2), ' ');
    const std::string inner_pad(static_cast<std::size_t>((indent + 1) * 2), ' ');

    if (val.is_object()) {
        const auto& obj = val.as_object();
        if (obj.empty()) {
            out << "{}";
            return;
        }
        out << "{\n";
        std::size_t idx = 0;
        for (const auto& kv : obj) {
            out << inner_pad << boost::json::serialize(boost::json::value(kv.key())) << ": ";
            PrettyWriteJson(out, kv.value(), indent + 1);
            if (++idx < obj.size()) {
                out << ",";
            }
            out << "\n";
        }
        out << pad << "}";
        return;
    }

    if (val.is_array()) {
        const auto& arr = val.as_array();
        if (arr.empty()) {
            out << "[]";
            return;
        }
        out << "[\n";
        for (std::size_t i = 0; i < arr.size(); ++i) {
            out << inner_pad;
            PrettyWriteJson(out, arr[i], indent + 1);
            if (i + 1 < arr.size()) {
                out << ",";
            }
            out << "\n";
        }
        out << pad << "]";
        return;
    }

    out << boost::json::serialize(val);
}

struct FlatLineRef {
    std::string script_name;
    std::size_t index_in_script = 0;
    std::string jp_text;
    std::string clean_jp_text;
    std::string speaker;
    bool needs_translation = false;
};

std::string ResolveSpeakerDisplay(std::string_view speaker,
                                  const Glossary& glossary,
                                  const TranslationCache& cache) {
    if (speaker.empty()) {
        return {};
    }
    if (const auto from_glossary = glossary.LookupSpeaker(speaker); from_glossary.has_value()) {
        return *from_glossary;
    }
    if (const auto* from_cache = cache.Find(speaker); from_cache != nullptr) {
        return *from_cache;
    }
    return std::string(speaker);
}

std::string FormatPromptLine(std::string_view speaker,
                             std::string_view text,
                             const Glossary& glossary,
                             const TranslationCache& cache) {
    const std::string speaker_display = ResolveSpeakerDisplay(speaker, glossary, cache);
    if (!speaker_display.empty()) {
        return "[" + speaker_display + "] " + std::string(text);
    }
    return "[NARRATION] " + std::string(text);
}

void WriteTranslatedTextFile(const std::string& translated_file,
                             const boost::json::value& extracted_root,
                             const TranslationCache& cache,
                             const Glossary& glossary) {
    if (translated_file.empty()) {
        return;
    }
    boost::json::value out_val = extracted_root;
    if (out_val.is_object()) {
        auto* scripts_container = &out_val.as_object();
        if (auto* scripts_sub = scripts_container->if_contains("scripts");
            scripts_sub != nullptr && scripts_sub->is_object()) {
            scripts_container = &scripts_sub->as_object();
        }
        for (auto& kv : *scripts_container) {
            boost::json::array* entries_arr = nullptr;
            if (kv.value().is_array()) {
                entries_arr = &kv.value().as_array();
            } else if (kv.value().is_object()) {
                auto& file_obj = kv.value().as_object();
                if (auto* s_it = file_obj.if_contains("strings");
                    s_it != nullptr && s_it->is_array()) {
                    entries_arr = &s_it->as_array();
                } else if (auto* l_it = file_obj.if_contains("lines");
                           l_it != nullptr && l_it->is_array()) {
                    entries_arr = &l_it->as_array();
                }
            }
            if (entries_arr == nullptr) {
                continue;
            }
            for (auto& item : *entries_arr) {
                if (!item.is_object()) {
                    continue;
                }
                auto& obj = item.as_object();
                std::string text;
                if (const auto* t = obj.if_contains("text"); t != nullptr && t->is_string()) {
                    text = std::string(t->as_string());
                }
                if (const auto* cached = cache.Find(text); cached != nullptr) {
                    obj["translated"] = *cached;
                    obj["translation"] = *cached;
                } else {
                    obj["translated"] = text;
                    obj["translation"] = text;
                }
                if (const auto* sp = obj.if_contains("speaker");
                    sp != nullptr && sp->is_string() && !sp->as_string().empty()) {
                    const std::string sp_str(sp->as_string());
                    const std::string sp_en = ResolveSpeakerDisplay(sp_str, glossary, cache);
                    if (!sp_en.empty()) {
                        obj["speaker_en"] = sp_en;
                    }
                }
            }
        }
    }

    std::ostringstream ss;
    PrettyWriteJson(ss, out_val, 0);
    ss << "\n";
    WriteFileAtomic(translated_file, ss.str());
}

void WriteLastBatchLog(const std::string& last_batch_log_file,
                       int batch_number,
                       int total_batches,
                       const std::vector<std::string>& batch_jp_keys,
                       const TranslationCache& cache) {
    if (last_batch_log_file.empty()) {
        return;
    }
    boost::json::array entries;
    for (const auto& jp : batch_jp_keys) {
        boost::json::object item;
        item["jp"] = jp;
        if (const auto* en = cache.Find(jp); en != nullptr) {
            item["en"] = *en;
        } else {
            item["en"] = "";
        }
        entries.push_back(std::move(item));
    }
    boost::json::object root;
    root["batch"] = batch_number;
    root["total_batches"] = total_batches;
    root["cache_size"] = static_cast<std::int64_t>(cache.Size());
    root["entries"] = std::move(entries);

    std::ostringstream ss;
    PrettyWriteJson(ss, root, 0);
    ss << "\n";
    WriteFileAtomic(last_batch_log_file, ss.str());
}

}  // namespace

const std::string* TranslationCache::Find(std::string_view key) const {
    const auto it = values.find(std::string(key));
    if (it == values.end()) {
        return nullptr;
    }
    return &it->second;
}

void TranslationCache::Set(std::string key, std::string value) {
    const auto it = values.find(key);
    if (it == values.end()) {
        order.push_back(key);
        values.emplace(std::move(key), std::move(value));
        return;
    }
    it->second = std::move(value);
}

bool TranslationCache::Erase(std::string_view key) {
    const auto it = values.find(std::string(key));
    if (it == values.end()) {
        return false;
    }
    values.erase(it);
    order.erase(std::remove(order.begin(), order.end(), key), order.end());
    return true;
}

bool TranslationCache::Contains(std::string_view key) const {
    return values.find(std::string(key)) != values.end();
}

bool NeedsTranslation(std::string_view text) {
    const std::string trimmed = TrimUnicode(text);
    if (trimmed.empty()) {
        return false;
    }
    const auto cps = DecodeCodepoints(trimmed);
    bool all_digits = true;
    bool has_jp = false;
    for (const std::uint32_t cp : cps) {
        if (!IsNumericOrWhitespace(cp)) {
            all_digits = false;
        }
        if (IsJapaneseChar(cp)) {
            has_jp = true;
        }
    }
    if (all_digits) {
        return false;
    }
    return has_jp;
}

TranslationCache LoadCache(const std::string& cache_file) {
    TranslationCache cache;
    const std::string raw = ReadWholeFile(cache_file);
    if (raw.empty()) {
        return cache;
    }
    boost::system::error_code ec;
    const boost::json::value val = boost::json::parse(raw, ec);
    if (ec || !val.is_object()) {
        return cache;
    }
    for (const auto& kv : val.as_object()) {
        if (!kv.value().is_string()) {
            continue;
        }
        cache.Set(std::string(kv.key()), std::string(kv.value().as_string()));
    }
    return cache;
}

void SaveCache(const std::string& cache_file, const TranslationCache& cache) {
    std::ostringstream ss;
    WriteJsonObjectOrdered(ss, cache.order, cache.values);
    WriteFileAtomic(cache_file, ss.str());
}

std::size_t CacheEntryCount(const std::string& cache_file) {
    return LoadCache(cache_file).Size();
}

bool RefuseCacheDiscard(const TranslateOptions& options,
                        std::size_t existing_entries,
                        std::string* reason_out) {
    if (!options.retranslate) {
        return false;
    }
    if (existing_entries == 0) {
        return false;
    }
    if (options.discard_cache) {
        return false;
    }
    if (reason_out != nullptr) {
        *reason_out = "Refusing to discard non-empty cache (" +
                      std::to_string(existing_entries) +
                      " entries) without --discard-cache";
    }
    return true;
}

bool IsFailedEntry(std::string_view key, std::string_view val) {
    if (val != key) {
        return false;
    }
    return NeedsTranslation(key);
}

std::size_t PurgeFailedEntries(TranslationCache& cache) {
    std::vector<std::string> to_remove;
    for (const auto& k : cache.order) {
        const auto it = cache.values.find(k);
        if (it != cache.values.end() && IsFailedEntry(k, it->second)) {
            to_remove.push_back(k);
        }
    }
    for (const auto& k : to_remove) {
        cache.Erase(k);
    }
    return to_remove.size();
}

bool ApplyLineResult(std::string_view jp_text,
                     const std::map<int, std::string>& batch_results,
                     int one_based_index,
                     TranslationCache& cache) {
    const auto it = batch_results.find(one_based_index);
    if (it == batch_results.end()) {
        return false;
    }
    const std::string cleaned = SanitizeAscii(StripSpeakerTags(it->second));
    if (cleaned.empty() || cleaned == jp_text) {
        return false;
    }
    cache.Set(std::string(jp_text), cleaned);
    return true;
}

void ValidateOptions(const TranslateOptions& options) {
    if (options.batch_size <= 0) {
        throw std::invalid_argument("batch_size must be positive");
    }
    if (options.max_batches < 0) {
        throw std::invalid_argument("max_batches must not be negative");
    }
    if (options.test_mode && options.test_batches <= 0) {
        throw std::invalid_argument("test_batches must be positive when test_mode is enabled");
    }
}

struct TranslateCore::Impl {
    ITranslationBackend* backend_override = nullptr;

    explicit Impl(ITranslationBackend* backend)
        : backend_override(backend) {}

    int Run(const TranslateOptions& options) {
        ValidateOptions(options);

        if (options.rebuild_table_only) {
            return build_tsv::RunBuild(options.translated_file, options.cache_file, options.tsv_file);
        }

        Glossary glossary;
        if (!options.tone_file.empty() && std::filesystem::exists(options.tone_file)) {
            glossary.LoadFromToneFile(options.tone_file);
        }

        const std::string extracted_raw = ReadWholeFile(options.extracted_file);
        if (extracted_raw.empty()) {
            std::cerr << "Error: Failed to read extracted_text.json at "
                      << options.extracted_file << "\n";
            return kExitError;
        }
        boost::system::error_code ec;
        const boost::json::value extracted_val = boost::json::parse(extracted_raw, ec);
        if (ec || !extracted_val.is_object()) {
            std::cerr << "Error: Invalid JSON in " << options.extracted_file << "\n";
            return kExitError;
        }

        SpeakerGate gate;
        const SpeakerCheckReport gate_report = gate.ValidateCorpus(extracted_val, glossary);
        if (!gate_report.passed) {
            std::cerr << "[SPEAKER-CHECK] FAIL (check " << gate_report.failed_check
                      << "): " << gate_report.reason << "\n";
            return kExitGateOrRefusal;
        }
        std::cout << "[SPEAKER-CHECK] PASS: " << gate_report.reason << "\n";

        const std::size_t existing_count = CacheEntryCount(options.cache_file);
        std::string refusal_reason;
        if (RefuseCacheDiscard(options, existing_count, &refusal_reason)) {
            std::cerr << refusal_reason << "\n";
            return kExitGateOrRefusal;
        }

        TranslationCache cache;
        if (!(options.retranslate && options.discard_cache)) {
            cache = LoadCache(options.cache_file);
            const std::size_t purged = PurgeFailedEntries(cache);
            if (purged > 0) {
                std::cout << "Purged " << purged << " failed entries from existing cache.\n";
                SaveCache(options.cache_file, cache);
            }
        }

        for (const auto& [jp_name, en_name] : glossary.OrderedSpeakerTranslations()) {
            if (!cache.Contains(jp_name)) {
                cache.Set(jp_name, en_name);
            }
        }

        std::vector<FlatLineRef> flat_lines;
        const auto* scripts_container = &extracted_val.as_object();
        if (const auto* scripts_sub = scripts_container->if_contains("scripts");
            scripts_sub != nullptr && scripts_sub->is_object()) {
            scripts_container = &scripts_sub->as_object();
        }
        for (const auto& kv : *scripts_container) {
            const boost::json::array* entries_arr = nullptr;
            if (kv.value().is_array()) {
                entries_arr = &kv.value().as_array();
            } else if (kv.value().is_object()) {
                const auto& file_obj = kv.value().as_object();
                if (const auto* s_it = file_obj.if_contains("strings");
                    s_it != nullptr && s_it->is_array()) {
                    entries_arr = &s_it->as_array();
                } else if (const auto* l_it = file_obj.if_contains("lines");
                           l_it != nullptr && l_it->is_array()) {
                    entries_arr = &l_it->as_array();
                }
            }
            if (entries_arr == nullptr) {
                continue;
            }
            const auto& arr = *entries_arr;
            for (std::size_t i = 0; i < arr.size(); ++i) {
                if (!arr[i].is_object()) {
                    continue;
                }
                const auto& obj = arr[i].as_object();
                FlatLineRef ref;
                ref.script_name = std::string(kv.key());
                ref.index_in_script = i;
                if (const auto* t = obj.if_contains("text"); t != nullptr && t->is_string()) {
                    ref.jp_text = std::string(t->as_string());
                }
                if (const auto* s = obj.if_contains("speaker"); s != nullptr && s->is_string()) {
                    ref.speaker = std::string(s->as_string());
                }
                ref.clean_jp_text = gate.DecomposeInlineControls(ref.jp_text).clean_text;
                if (ref.clean_jp_text.empty()) {
                    ref.clean_jp_text = ref.jp_text;
                }
                ref.needs_translation = NeedsTranslation(ref.jp_text);
                flat_lines.push_back(std::move(ref));
            }
        }

        // Scan forward across all scripts for unique uncached translatable lines.
        std::vector<std::size_t> to_translate_indices;
        std::unordered_set<std::string> queued_unique_jp;
        for (std::size_t i = 0; i < flat_lines.size(); ++i) {
            if (!flat_lines[i].needs_translation) {
                continue;
            }
            if (cache.Contains(flat_lines[i].jp_text)) {
                continue;
            }
            if (queued_unique_jp.insert(flat_lines[i].jp_text).second) {
                to_translate_indices.push_back(i);
            }
        }

        int effective_max_batches = options.max_batches;
        if (options.test_mode) {
            effective_max_batches = std::max(1, options.test_batches);
        }

        const auto batch_sz = static_cast<std::size_t>(options.batch_size);
        std::size_t total_batches =
            (to_translate_indices.size() + batch_sz - 1U) / batch_sz;
        if (effective_max_batches > 0 &&
            total_batches > static_cast<std::size_t>(effective_max_batches)) {
            total_batches = static_cast<std::size_t>(effective_max_batches);
        }

        std::cout << "Total flat lines: " << flat_lines.size()
                  << " | Cached entries: " << cache.Size()
                  << " | Unique uncached lines: " << to_translate_indices.size()
                  << " | Batches to run: " << total_batches << "\n";

        std::unique_ptr<GeminiClient> owned_client;
        ITranslationBackend* backend = backend_override;
        if (backend == nullptr && total_batches > 0) {
            const std::string api_key = LoadGeminiApiKey(options.project_dir);
            if (api_key.empty()) {
                std::cerr << "Error: GEMINI_API_KEY not found in environment or ~/.gemini/.env\n";
                return kExitError;
            }
            owned_client = std::make_unique<GeminiClient>(
                api_key, kDefaultModel, options.project_dir);
            backend = owned_client.get();
        }

        std::size_t batches_executed = 0;
        for (std::size_t start = 0;
             start < to_translate_indices.size() && batches_executed < total_batches;
             start += batch_sz) {
            const std::size_t end =
                std::min(start + batch_sz, to_translate_indices.size());

            std::vector<std::size_t> batch_flat_indices;
            batch_flat_indices.reserve(end - start);
            std::vector<std::string> prompt_lines;
            prompt_lines.reserve(end - start);
            std::vector<std::string> batch_jp_keys;
            batch_jp_keys.reserve(end - start);

            for (std::size_t k = start; k < end; ++k) {
                const std::size_t flat_idx = to_translate_indices[k];
                const auto& ref = flat_lines[flat_idx];
                batch_flat_indices.push_back(flat_idx);
                batch_jp_keys.push_back(ref.jp_text);
                prompt_lines.push_back(
                    FormatPromptLine(ref.speaker, ref.clean_jp_text, glossary, cache));
            }

            std::vector<std::string> prev_context;
            const std::size_t first_flat_idx = batch_flat_indices.front();
            const std::size_t ctx_start =
                (first_flat_idx > kContextWindowLines)
                    ? (first_flat_idx - kContextWindowLines)
                    : 0U;
            for (std::size_t c = ctx_start; c < first_flat_idx; ++c) {
                const auto& cref = flat_lines[c];
                const auto* cached_en = cache.Find(cref.jp_text);
                const std::string_view ctx_body =
                    (cached_en != nullptr)
                        ? std::string_view(*cached_en)
                        : std::string_view(cref.clean_jp_text);
                prev_context.push_back(
                    FormatPromptLine(cref.speaker, ctx_body, glossary, cache));
            }

            std::vector<std::string> ctx_after;
            const std::size_t last_flat_idx = batch_flat_indices.back();
            const std::size_t after_end =
                std::min(flat_lines.size(), last_flat_idx + 1U + kContextAfterLines);
            for (std::size_t c = last_flat_idx + 1U; c < after_end; ++c) {
                const auto& cref = flat_lines[c];
                ctx_after.push_back(
                    FormatPromptLine(cref.speaker, cref.clean_jp_text, glossary, cache));
            }

            std::map<int, std::string> results;
            int attempt = 0;
            while (attempt < kMaxApiRetries) {
                try {
                    results = TranslateBatchWithFallback(
                        *backend, prompt_lines, prev_context, ctx_after);
                    break;
                } catch (const RateLimitError& err) {
                    ++attempt;
                    if (attempt >= kMaxApiRetries) {
                        std::cerr << "Error: Rate limit retries exhausted: " << err.what() << "\n";
                        return kExitError;
                    }
                    const int wait_sec = kInitialRetryBackoffSec * (1 << (attempt - 1));
                    std::cerr << "Rate limit hit (" << err.what() << "), waiting "
                              << wait_sec << "s before retry " << attempt << "...\n";
                    std::this_thread::sleep_for(std::chrono::seconds(wait_sec));
                } catch (const ApiError& err) {
                    ++attempt;
                    if (attempt >= kMaxApiRetries) {
                        std::cerr << "Error: API retries exhausted: " << err.what() << "\n";
                        return kExitError;
                    }
                    const int wait_sec = kInitialRetryBackoffSec * kBackoffMultiplier * attempt;
                    std::cerr << "API error (" << err.what() << "), waiting "
                              << wait_sec << "s before retry " << attempt << "...\n";
                    std::this_thread::sleep_for(std::chrono::seconds(wait_sec));
                }
            }

            std::size_t applied_in_batch = 0;
            for (std::size_t idx = 0; idx < batch_flat_indices.size(); ++idx) {
                const auto& ref = flat_lines[batch_flat_indices[idx]];
                if (ApplyLineResult(
                        ref.jp_text, results, static_cast<int>(idx + 1U), cache)) {
                    ++applied_in_batch;
                }
            }

            ++batches_executed;
            SaveCache(options.cache_file, cache);
            WriteLastBatchLog(options.last_batch_log_file,
                              static_cast<int>(batches_executed),
                              static_cast<int>(total_batches),
                              batch_jp_keys,
                              cache);
            WriteTranslatedTextFile(options.translated_file, extracted_val, cache, glossary);

            std::cout << "[Batch " << batches_executed << "/" << total_batches
                      << "] Applied " << applied_in_batch << "/" << batch_flat_indices.size()
                      << " lines | Cache total: " << cache.Size() << "\n";
        }

        if (batches_executed == 0) {
            SaveCache(options.cache_file, cache);
            WriteTranslatedTextFile(options.translated_file, extracted_val, cache, glossary);
        }

        if (!options.tsv_file.empty()) {
            const int tsv_rc = build_tsv::RunBuild(
                options.translated_file, options.cache_file, options.tsv_file);
            if (tsv_rc != 0) {
                return tsv_rc;
            }
        }

        return kExitOk;
    }
};

TranslateCore::TranslateCore()
    : impl_(std::make_unique<Impl>(nullptr)) {}

TranslateCore::TranslateCore(ITranslationBackend* backend_override)
    : impl_(std::make_unique<Impl>(backend_override)) {}

TranslateCore::TranslateCore(TranslateCore&& other) noexcept = default;
TranslateCore& TranslateCore::operator=(TranslateCore&& other) noexcept = default;
TranslateCore::~TranslateCore() = default;

int TranslateCore::Run(const TranslateOptions& options) {
    return impl_->Run(options);
}

int TranslateAll(const TranslateOptions& options,
                 ITranslationBackend* backend_override) {
    TranslateCore core(backend_override);
    return core.Run(options);
}

}  // namespace oretsuba::translate
