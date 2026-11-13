// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "cache/cache_core.h"

#include "build_tsv/build_tsv.h"
#include "cache/glossary.h"
#include "cache/speaker_gate.h"
#include "common/util.h"

#include <boost/regex.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <tuple>

namespace crc::translate {

namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

constexpr int kBatchSize = 150;
constexpr std::size_t kMinTranslatableShortLen = 3;
constexpr std::size_t kMaxSpeakerCodepoints = 20;
constexpr std::size_t kMaxSpeakerPrefixCodepoints = 10;
constexpr std::size_t kCrlfCodepoints = 2;
constexpr std::size_t kCrlfByteLength = 2;
constexpr unsigned char kNonAsciiFloor = 0x80;
constexpr int kCacheJsonIndent = 2;
constexpr int kTranslatedJsonIndent = 1;
constexpr int kSpeakerGateFailedExitCode = 2;
constexpr int kRefusedTestExitCode = 1;
constexpr int kOkExitCode = 0;
constexpr std::size_t kPercentScale = 100;
constexpr double kBytesPerMegabyte = 1024.0 * 1024.0;
constexpr std::size_t kProgressBufSize = 160;
constexpr std::size_t kSizeBufSize = 64;

constexpr char32_t kHiraganaFirst = U'ぁ' - 1;  // 0x3040
constexpr char32_t kHiraganaLast = 0x309F;
constexpr char32_t kKatakanaFirst = 0x30A0;
constexpr char32_t kKatakanaLast = 0x30FF;
constexpr char32_t kCjkUnifiedFirst = 0x4E00;
constexpr char32_t kCjkUnifiedLast = 0x9FFF;
constexpr char32_t kCjkExtAFirst = 0x3400;
constexpr char32_t kCjkExtALast = 0x4DBF;

std::size_t char_len(const std::string& utf8) {
    std::size_t n = 0;
    std::size_t i = 0;
    while (i < utf8.size()) {
        utf8_next(utf8, i);
        ++n;
    }
    return n;
}

// Byte offset of codepoint index `n`.  Callers slice by codepoint (a JP name
// plus its CRLF), so the cut is located through a UTF-8 walk rather than by
// adding byte counts.
std::size_t byte_offset_of_cp(const std::string& utf8, std::size_t n) {
    std::size_t i = 0;
    std::size_t seen = 0;
    while (i < utf8.size() && seen < n) {
        utf8_next(utf8, i);
        ++seen;
    }
    return i;
}

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) {
        return s;
    }
    std::string out;
    out.reserve(s.size());
    std::size_t pos = 0;
    for (;;) {
        const std::size_t hit = s.find(from, pos);
        if (hit == std::string::npos) {
            out.append(s, pos, std::string::npos);
            return out;
        }
        out.append(s, pos, hit - pos);
        out += to;
        pos = hit + from.size();
    }
}

std::string json_str(const bj::object& o, const char* key) {
    if (const auto* v = o.if_contains(key)) {
        if (v->is_string()) {
            return std::string(v->get_string());
        }
    }
    return {};
}

// Returns (translated_count, stopped_early).
std::pair<long long, bool> translate_file(const bj::object& fdata, Cache& cache, int batch_size) {
    const std::vector<std::size_t> translatable = translatable_indices(fdata);
    if (translatable.empty()) {
        return {0, false};
    }

    const auto& strings = fdata.at("strings").get_array();
    long long translated_count = 0;

    for (std::size_t batch_start = 0; batch_start < translatable.size();
         batch_start += static_cast<std::size_t>(batch_size)) {
        const std::size_t batch_end = std::min(
            translatable.size(), batch_start + static_cast<std::size_t>(batch_size));

        for (std::size_t k = batch_start; k < batch_end; ++k) {
            const std::size_t idx = translatable[k];
            const auto& s = strings[idx].get_object();
            const std::string text = json_str(s, "text");
            if (!cache.get(text)) {
                throw std::runtime_error(
                    "Offline build: missing cached translation; live API client removed for finished game");
            }
            ++translated_count;
        }
    }

    return {translated_count, false};
}

}  // namespace

bool has_real_japanese(const std::string& text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const char32_t cp = utf8_next(text, i);
        if ((cp >= kHiraganaFirst && cp <= kHiraganaLast) ||
            (cp >= kKatakanaFirst && cp <= kKatakanaLast) ||
            (cp >= kCjkUnifiedFirst && cp <= kCjkUnifiedLast) ||
            (cp >= kCjkExtAFirst && cp <= kCjkExtALast)) {
            return true;
        }
    }
    return false;
}

bool needs_translation(const std::string& text) {
    if (text.empty() || trim(text).empty()) {
        return false;
    }
    if (char_len(text) <= kMinTranslatableShortLen && !has_real_japanese(text)) {
        return false;
    }
    return has_real_japanese(text);
}

std::vector<std::size_t> translatable_indices(const bj::object& fdata) {
    std::vector<std::size_t> out;
    const auto* strings = fdata.if_contains("strings");
    if (!strings || !strings->is_array()) {
        return out;
    }
    const auto& arr = strings->get_array();
    for (std::size_t i = 0; i < arr.size(); ++i) {
        const auto& s = arr[i].get_object();
        const std::string text = json_str(s, "text");
        const std::string type = json_str(s, "type");
        if (needs_translation(text) && (type == "dialogue" || type == "narration")) {
            out.push_back(i);
        }
    }
    return out;
}

std::string extract_speaker(const bj::object& s) {
    std::string raw = trim(json_str(s, "speaker"));
    if (raw.empty()) {
        const std::string text = json_str(s, "text");
        const std::size_t crlf = text.find("\r\n");
        if (crlf != std::string::npos) {
            raw = trim(text.substr(0, crlf));
        }
    }
    if (!raw.empty() && char_len(raw) <= kMaxSpeakerCodepoints) {
        const auto& map = name_translations();
        const auto it = map.find(raw);
        return it == map.end() ? raw : it->second;
    }
    return "NARRATION";
}

std::string sanitize_ascii(const std::string& text) {
    std::string s = text;
    s = replace_all(s, "—", "--");
    s = replace_all(s, "–", "--");
    s = replace_all(s, "…", "...");
    s = replace_all(s, "‘", "'");
    s = replace_all(s, "’", "'");
    s = replace_all(s, "“", "\"");
    s = replace_all(s, "”", "\"");
    s = replace_all(s, "　", " ");
    return s;
}

std::string postprocess(const std::string& original, const std::string& translated) {
    if (translated.empty() || translated == original) {
        return translated;
    }
    std::string t = sanitize_ascii(translated);

    // Re-attach the speaker plate.  Only the first \r\n-segment counts, and
    // only when it is a short name the glossary actually knows -- otherwise a
    // narrative line break would be mistaken for a speaker prefix.
    const std::size_t crlf = original.find("\r\n");
    if (crlf != std::string::npos) {
        const std::string jp_name = trim(original.substr(0, crlf));
        const auto& map = name_translations();
        const auto it = map.find(jp_name);
        if (!jp_name.empty() && char_len(jp_name) <= kMaxSpeakerPrefixCodepoints &&
            it != map.end()) {
            const std::string& en_name = it->second;
            if (t.rfind(en_name + "\r\n", 0) != 0) {
                if (t.rfind(jp_name + "\r\n", 0) == 0) {
                    t = t.substr(byte_offset_of_cp(t, char_len(jp_name) + kCrlfCodepoints));
                }
                t = en_name + "\r\n" + t;
            }
        }
    }
    const bool orig_crlf_end =
        original.size() >= kCrlfByteLength &&
        original.compare(original.size() - kCrlfByteLength, kCrlfByteLength, "\r\n") == 0;
    const bool tr_crlf_end =
        t.size() >= kCrlfByteLength &&
        t.compare(t.size() - kCrlfByteLength, kCrlfByteLength, "\r\n") == 0;
    if (orig_crlf_end && !tr_crlf_end) {
        t = trim_right(t) + "\r\n";
    }
    return t;
}

Cache load_cache(const std::string& cache_file) {
    Cache cache;
    if (!fs::exists(fs::u8path(cache_file))) {
        return cache;
    }
    const bj::value root = json_parse_file(cache_file);
    if (!root.is_object()) {
        throw std::runtime_error("cannot parse " + cache_file +
                                 ": a translation cache must be a JSON object");
    }
    for (const auto& kv : root.get_object()) {
        if (kv.value().is_string()) {
            cache.set(std::string(kv.key()), std::string(kv.value().get_string()));
        }
    }
    return cache;
}

std::size_t cache_entry_count(const std::string& cache_file) {
    return load_cache(cache_file).size();
}

std::optional<std::string> refuse_cache_discard(std::size_t cache_entries,
                                                const std::string& flag,
                                                bool discard_cache) {
    if (discard_cache || cache_entries <= CACHE_DISCARD_THRESHOLD) {
        return std::nullopt;
    }
    return flag + " deletes the translation cache, which holds " +
           std::to_string(cache_entries) +
           " lines that were paid for. Nothing in this pipeline can rebuild "
           "them.\n"
           "        To delete them anyway, add --discard-cache.";
}

void save_cache(const Cache& cache, const std::string& cache_file) {
    bj::object o;
    for (const auto& [jp, en] : cache.items()) {
        o[jp] = en;
    }
    write_file_atomic_text(cache_file, json_pretty(bj::value(std::move(o)), kCacheJsonIndent));
}

bool is_failed_entry(const std::string& jp, const std::string& en) {
    if (jp != en) {
        return false;
    }
    for (const char c : jp) {
        if (static_cast<unsigned char>(c) >= kNonAsciiFloor) {
            return true;
        }
    }
    return false;
}

std::size_t purge_failed_entries(Cache& cache) {
    std::size_t removed = 0;
    Cache kept;
    for (const auto& [jp, en] : cache.items()) {
        if (is_failed_entry(jp, en)) {
            ++removed;
            continue;
        }
        kept.set(jp, en);
    }
    if (removed != 0) {
        cache = std::move(kept);
    }
    return removed;
}

std::map<int, std::string> parse_numbered_response(const std::string& response_text) {
    static const boost::regex kTagRegex(R"(^\[(?:NARRATION|[A-Za-z\s()'?]+)\]\s*)");
    static const boost::regex kNumberRegex(R"(^(\d+)[.):\s]+\s*(.*))");
    std::map<int, std::string> translations;

    std::size_t pos = 0;
    while (pos <= response_text.size()) {
        const std::size_t nl = response_text.find('\n', pos);
        std::string line = response_text.substr(
            pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? response_text.size() + 1 : nl + 1;

        line = trim(line);
        if (line.empty()) {
            continue;
        }
        line = boost::regex_replace(line, kTagRegex, "");
        boost::smatch m;
        if (boost::regex_match(line, m, kNumberRegex)) {
            std::string text = trim(m[2].str());
            text = replace_all(text, "\\r\\n", "\r\n");
            text = replace_all(text, "\\n", "\n");
            translations[std::stoi(m[1].str())] = text;
        }
    }
    return translations;
}

int translate_all(const TranslateOptions& opt) {
    log_info("Batch size: " + std::to_string(kBatchSize) + " lines per request");

    log_info("\nLoading extracted text...");
    bj::value all_data_v = json_parse_file(opt.input_file);
    bj::object& all_data = all_data_v.get_object();

    log_info("\nRunning speaker gate...");
    const SpeakerCheckReport gate = run_speaker_checks(all_data);
    if (!gate.passed) {
        log_info("\n[ERROR] speaker check " + std::to_string(gate.failed_check) +
                 " failed: " + gate.reason);
        log_info("        Nothing was translated and nothing was written.");
        return kSpeakerGateFailedExitCode;
    }

    if (opt.test_batches != 0) {
        log_info("[ERROR] offline build: network API calls are disabled -- refusing to discard the cache");
        return kRefusedTestExitCode;
    }

    log_info("Loading translation cache...");
    Cache cache = load_cache(opt.cache_file);
    log_info("  Cache has " + std::to_string(cache.size()) + " entries");
    const std::size_t purged = purge_failed_entries(cache);
    log_info("  Failed entries purged: " + std::to_string(purged) +
             (purged ? "  (those lines re-queue)" : ""));

    for (const auto& [jp, en] : name_translations_ordered()) {
        cache.set(jp, en);
    }

    std::vector<std::string> file_order;
    for (const auto& f : story_order()) {
        if (all_data.if_contains(f)) {
            file_order.push_back(f);
        }
    }
    for (const auto& kv : all_data) {
        const std::string name(kv.key());
        if (std::find(file_order.begin(), file_order.end(), name) == file_order.end()) {
            file_order.push_back(name);
        }
    }

    std::vector<std::tuple<std::string, std::size_t, std::size_t>> file_stats;
    std::size_t total_translatable = 0;
    std::size_t total_uncached = 0;
    for (const auto& fname : file_order) {
        const auto& fdata = all_data.at(fname).get_object();
        const auto idxs = translatable_indices(fdata);
        const auto& strings = fdata.at("strings").get_array();
        std::size_t uncached = 0;
        for (const std::size_t i : idxs) {
            if (!cache.contains(json_str(strings[i].get_object(), "text"))) {
                ++uncached;
            }
        }
        file_stats.emplace_back(fname, idxs.size(), uncached);
        total_translatable += idxs.size();
        total_uncached += uncached;
    }

    log_info("\nTranslation summary:");
    log_info("  Files to process: " + std::to_string(file_stats.size()));
    log_info("  Total translatable lines: " + std::to_string(total_translatable));
    log_info("  Need translating: " + std::to_string(total_uncached) +
             " (cached: " + std::to_string(total_translatable - total_uncached) + ")");

    if (total_uncached == 0) {
        log_info("  Everything is already cached!");
    } else {
        log_info("\nTranslating...");
        std::size_t done_lines = 0;
        const auto start = std::chrono::steady_clock::now();

        for (const auto& [fname, line_count, uncached_count] : file_stats) {
            if (uncached_count == 0) {
                done_lines += line_count;
                continue;
            }

            log_info("\n  [" + fname + "] " + std::to_string(uncached_count) + " uncached / " +
                     std::to_string(line_count) + " translatable");

            const auto [count, stopped] =
                translate_file(all_data.at(fname).get_object(), cache, kBatchSize);
            (void)count;
            (void)stopped;
            done_lines += line_count;

            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            const double rate = elapsed > 0.0 ? static_cast<double>(done_lines) / elapsed : 0.0;
            char buf[kProgressBufSize];
            std::snprintf(buf, sizeof(buf), "  Progress: %zu/%zu (%zu%%) | %.1f l/s", done_lines,
                          total_translatable,
                          total_translatable ? done_lines * kPercentScale / total_translatable : 0,
                          rate);
            log_info(buf);
        }

        save_cache(cache, opt.cache_file);
        log_info("\nTranslation done! Cache: " + std::to_string(cache.size()) + " entries");
    }

    log_info("\nBuilding translated text...");
    for (auto& kv : all_data) {
        auto* strings = kv.value().get_object().if_contains("strings");
        if (!strings || !strings->is_array()) {
            continue;
        }
        for (auto& sv : strings->get_array()) {
            auto& s = sv.get_object();
            const std::string text = json_str(s, "text");
            const std::string* en = cache.get(text);
            s["translated"] = (en && *en != text) ? *en : text;
        }
    }

    write_file_text(opt.output_file, json_pretty(all_data_v, kTranslatedJsonIndent));
    char sz[kSizeBufSize];
    std::snprintf(sz, sizeof(sz), "%.1f MB",
                  static_cast<double>(fs::file_size(fs::u8path(opt.output_file))) /
                      kBytesPerMegabyte);
    log_info("Saved to " + opt.output_file + " (" + sz + ")");

    if (opt.test_batches != 0) {
        log_info("\n--test " + std::to_string(opt.test_batches) +
                 ": skipping the runtime table "
                 "(smoke artifact is test/translation_run_*.json)");
        return kOkExitCode;
    }

    log_info("\nBuilding the runtime translation table...");
    return build_tsv::run_build(opt.output_file, opt.tsv_file, opt.game_tsv_file);
}

}  // namespace crc::translate
