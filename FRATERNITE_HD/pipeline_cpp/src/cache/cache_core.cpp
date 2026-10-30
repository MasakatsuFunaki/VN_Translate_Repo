// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "cache/cache_core.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <system_error>

#include "build_tsv/build_tsv.h"
#include "cache/glossary.h"
#include "cache/speaker_gate.h"
#include "common/util.h"

namespace frat::translate {

namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

constexpr char32_t kOpenBracket = 0x300C;   // 「
constexpr char32_t kCloseBracket = 0x300D;  // 」
constexpr char32_t kOpenDouble = 0x300E;    // 『

std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
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
    if (auto* v = o.if_contains(key))
        if (v->is_string()) return std::string(v->get_string());
    return {};
}

std::string nums_repr(const std::vector<long long>& v) {
    std::string out = "[";
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out += ", ";
        out += std::to_string(v[i]);
    }
    return out + "]";
}

bool is_small_kana(char32_t cp) {
    switch (cp) {
        case 0x3041: case 0x3043: case 0x3045: case 0x3047: case 0x3049:  // ぁぃぅぇぉ
        case 0x3063: case 0x3083: case 0x3085: case 0x3087: case 0x308E:  // っゃゅょゎ
        case 0x3095: case 0x3096:                                         // ゕゖ
        case 0x30A1: case 0x30A3: case 0x30A5: case 0x30A7: case 0x30A9:  // ァィゥェォ
        case 0x30C3: case 0x30E3: case 0x30E5: case 0x30E7: case 0x30EE:  // ッャュョヮ
        case 0x30F5: case 0x30F6:                                         // ヵヶ
            return true;
        default:
            return false;
    }
}

// A small kana never stands alone as a word, so a string of nothing but small
// kana is a cell of a character table rather than a message.  Whether the
// model romanises such a cell or echoes it back is arbitrary, and an echo can
// never be cached -- the JP-in-EN guard rejects it -- so an echoed cell is
// re-sent on every run and keeps the API open for a script already translated.
bool is_small_kana_only(const std::string& text) {
    std::size_t i = 0;
    bool any = false;
    while (i < text.size()) {
        if (!is_small_kana(utf8_next(text, i))) return false;
        any = true;
    }
    return any;
}

}  // namespace

// Defined here because glossary.cpp is a generated table dump.
const std::vector<std::pair<std::string, std::string>>& names_by_len_desc() {
    static const std::vector<std::pair<std::string, std::string>> data = [] {
        std::vector<std::pair<std::string, std::string>> v = name_translations_ordered();
        // Stable: equal-length keys keep declaration order.
        std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
            return char_len(a.first) > char_len(b.first);
        });
        return v;
    }();
    return data;
}

std::optional<std::string> validate_options(int batch_size, int test_n, int max_batches) {
    if (batch_size < 1)
        return "--batch must be at least 1 (got " + std::to_string(batch_size) + ")";
    if (test_n < 0)
        return "--test cannot be negative (got " + std::to_string(test_n) + ")";
    if (max_batches < 0)
        return "--max-batches cannot be negative (got " + std::to_string(max_batches) +
               "); use 0 for no limit";
    return std::nullopt;
}

bool has_real_japanese(const std::string& text) { return jp_char_count(text) > 0; }

bool needs_translation(const std::string& text) {
    if (text.empty() || trim(text).empty()) return false;
    if (is_small_kana_only(trim(text))) return false;
    return has_real_japanese(text);
}

std::vector<std::size_t> translatable_indices(const bj::object& fdata) {
    std::vector<std::size_t> out;
    const auto* strings = fdata.if_contains("strings");
    if (!strings || !strings->is_array()) return out;
    const auto& arr = strings->get_array();
    for (std::size_t i = 0; i < arr.size(); ++i) {
        const auto& s = arr[i].get_object();
        const std::string text = json_str(s, "text");
        const std::string type = json_str(s, "type");
        if (needs_translation(text) && (type == "dialogue" || type == "narrative" ||
                                        type == "name" || type == "menu"))
            out.push_back(i);
    }
    return out;
}

std::optional<std::string> inline_speaker_match(const std::string& text) {
    // Complement of [　-〿「『（()0-9a-zA-Z\s].
    const auto allowed = [](char32_t cp) {
        if (cp >= 0x3000 && cp <= 0x303F) return false;
        if (cp == 0xFF08 || cp == '(' || cp == ')') return false;
        if (cp >= '0' && cp <= '9') return false;
        if (cp >= 'a' && cp <= 'z') return false;
        if (cp >= 'A' && cp <= 'Z') return false;
        return !is_unicode_space(cp);
    };
    const std::vector<char32_t> cps = utf8_decode(text);
    std::size_t n = 0;
    while (n < cps.size() && allowed(cps[n])) ++n;
    if (n == 0 || n > 12 || n >= cps.size()) return std::nullopt;
    if (cps[n] != kOpenBracket && cps[n] != kOpenDouble) return std::nullopt;
    return utf8_encode(std::vector<char32_t>(cps.begin(),
                                             cps.begin() + static_cast<std::ptrdiff_t>(n)));
}

std::string extract_speaker(const std::string& text) {
    const std::size_t crlf = text.find("\r\n");
    if (crlf != std::string::npos) {
        const std::string name = trim(text.substr(0, crlf));
        // Unknown names fall back to raw JP, not NARRATION.
        if (!name.empty() && char_len(name) <= 20) {
            const auto& map = name_translations();
            auto it = map.find(name);
            return it == map.end() ? name : it->second;
        }
    }
    if (auto m = inline_speaker_match(text)) {
        const auto& map = name_translations();
        auto it = map.find(*m);
        if (it != map.end()) return it->second;
    }
    return "NARRATION";
}

std::string sanitize_ascii(const std::string& text) {
    std::string s = text;
    s = replace_all(s, "\xE2\x80\x94", "--");   // em dash
    s = replace_all(s, "\xE2\x80\x93", "--");   // en dash
    s = replace_all(s, "\xE2\x80\xA6", "...");  // horizontal ellipsis
    s = replace_all(s, "\xE2\x80\x98", "'");    // left single curly quote
    s = replace_all(s, "\xE2\x80\x99", "'");    // right single curly quote
    s = replace_all(s, "\xE2\x80\x9C", "\"");   // left double curly quote
    s = replace_all(s, "\xE2\x80\x9D", "\"");   // right double curly quote
    s = replace_all(s, "\xE3\x80\x80", " ");    // ideographic space
    return s;
}

bool en_has_japanese_content(const std::string& en) {
    std::size_t i = 0;
    while (i < en.size()) {
        const char32_t cp = utf8_next(en, i);
        if ((cp >= 0x3041 && cp <= 0x3096) ||   // hiragana letters
            (cp >= 0x30A1 && cp <= 0x30FA) ||   // katakana letters
            (cp >= 0x4E00 && cp <= 0x9FFF))     // kanji
            return true;
    }
    return false;
}

std::string postprocess(const std::string& original, const std::string& translated) {
    if (translated.empty() || translated == original) return translated;
    std::string t = sanitize_ascii(translated);

    // Romanise a leading JP name. Longest-first so 園田 doesn't eat into 園田Ｈ.
    const std::string open = utf8_encode_cp(kOpenBracket);
    for (const auto& [jp_name, en_name] : names_by_len_desc()) {
        if (en_name.empty()) continue;
        const std::string prefix = jp_name + open;
        if (t.rfind(prefix, 0) == 0) {
            t = en_name + open + t.substr(prefix.size());
            break;
        }
    }
    // Re-attach dropped brackets.
    if (auto m = inline_speaker_match(original)) {
        if (t.find(open) == std::string::npos &&
            t.find(utf8_encode_cp(kOpenDouble)) == std::string::npos) {
            const auto& map = name_translations();
            auto it = map.find(*m);
            if (it != map.end())
                t = it->second + open + trim(t) + utf8_encode_cp(kCloseBracket);
        }
    }
    const bool orig_crlf_end =
        original.size() >= 2 && original.compare(original.size() - 2, 2, "\r\n") == 0;
    const bool tr_crlf_end = t.size() >= 2 && t.compare(t.size() - 2, 2, "\r\n") == 0;
    if (orig_crlf_end && !tr_crlf_end) t = trim_right(t) + "\r\n";
    return t;
}

Cache load_cache(const std::string& cache_file) {
    Cache cache;
    if (!fs::exists(fs::u8path(cache_file))) return cache;
    bj::value root = json_parse_file(cache_file);
    if (!root.is_object())
        throw std::runtime_error("cannot parse " + cache_file +
                                 ": a translation cache must be a JSON object");
    for (const auto& kv : root.get_object())
        if (kv.value().is_string())
            cache.set(std::string(kv.key()), std::string(kv.value().get_string()));
    return cache;
}

std::size_t cache_entry_count(const std::string& cache_file) {
    return load_cache(cache_file).size();
}

std::optional<std::string> refuse_cache_discard(std::size_t cache_entries,
                                                const std::string& flag,
                                                bool discard_cache) {
    if (discard_cache || cache_entries <= CACHE_DISCARD_THRESHOLD)
        return std::nullopt;
    return flag + " deletes the translation cache, which holds " +
           std::to_string(cache_entries) +
           " lines that were paid for. Nothing in this pipeline can rebuild "
           "them.\n"
           "        To cap a run and keep them, use --max-batches N instead.\n"
           "        To delete them anyway, add --discard-cache.";
}

void save_cache(const Cache& cache, const std::string& cache_file) {
    bj::object o;
    for (const auto& [jp, en] : cache.items()) o[jp] = en;
    write_file_atomic_text(cache_file, json_pretty(bj::value(std::move(o)), 2));
}

bool is_failed_entry(const std::string& jp, const std::string& en) {
    if (jp != en) return false;
    for (char c : jp)
        if (static_cast<unsigned char>(c) >= 0x80) return true;
    return false;
}

std::size_t purge_failed_entries(Cache& cache) {
    std::size_t removed = 0;
    Cache kept;
    for (const auto& [jp, en] : cache.items()) {
        if (is_failed_entry(jp, en)) ++removed;
        else kept.set(jp, en);
    }
    if (removed) cache = std::move(kept);
    return removed;
}

// --------------------------------------------------------------------------- //
// Response parsing
// --------------------------------------------------------------------------- //

namespace {

// Strip a leading [SPEAKER] tag. Accepts '&' for names like "R&B".
std::string strip_speaker_tag(const std::string& line) {
    const std::vector<char32_t> cps = utf8_decode(line);
    if (cps.empty() || cps[0] != '[') return line;
    const auto tag_char = [](char32_t cp) {
        return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || cp == '(' ||
               cp == ')' || cp == '\'' || cp == '&' || cp == '?' || is_unicode_space(cp);
    };
    std::size_t i = 1;
    while (i < cps.size() && tag_char(cps[i])) ++i;
    if (i == 1 || i >= cps.size() || cps[i] != ']') return line;
    ++i;
    while (i < cps.size() && is_unicode_space(cps[i])) ++i;
    return utf8_encode(std::vector<char32_t>(cps.begin() + static_cast<std::ptrdiff_t>(i),
                                             cps.end()));
}

// Accepts ASCII and fullwidth digits (the model sometimes uses fullwidth).
bool match_numbered(const std::string& line, long long& num, std::string& rest) {
    const std::vector<char32_t> cps = utf8_decode(line);
    const auto digit_val = [](char32_t cp) -> int {
        if (cp >= '0' && cp <= '9') return static_cast<int>(cp - '0');
        if (cp >= 0xFF10 && cp <= 0xFF19) return static_cast<int>(cp - 0xFF10);
        return -1;
    };
    std::size_t i = 0;
    bool overflow = false;
    long long value = 0;
    while (i < cps.size() && digit_val(cps[i]) >= 0) {
        if (value > (1LL << 40)) overflow = true;  // digits keep coming: clamp
        if (!overflow) value = value * 10 + digit_val(cps[i]);
        ++i;
    }
    if (i == 0) return false;
    const auto sep_char = [](char32_t cp) {
        return cp == '.' || cp == ')' || cp == ':' || is_unicode_space(cp);
    };
    std::size_t j = i;
    while (j < cps.size() && sep_char(cps[j])) ++j;
    if (j == i) return false;  // [.):\s]+ needs at least one
    while (j < cps.size() && is_unicode_space(cps[j])) ++j;
    // Overflow -> -1 so the sequence check rejects the whole batch.
    num = overflow ? -1 : value;
    rest = utf8_encode(std::vector<char32_t>(cps.begin() + static_cast<std::ptrdiff_t>(j),
                                             cps.end()));
    return true;
}

}  // namespace

std::map<int, std::string> parse_translations(const std::string& response_text,
                                              std::size_t expected_count) {
    std::vector<std::pair<long long, std::string>> parsed;
    std::size_t pos = 0;
    while (pos <= response_text.size()) {
        const std::size_t nl = response_text.find('\n', pos);
        std::string line = response_text.substr(
            pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? response_text.size() + 1 : nl + 1;

        line = trim(line);
        if (line.empty()) continue;
        line = strip_speaker_tag(line);
        long long num = 0;
        std::string rest;
        if (!match_numbered(line, num, rest)) continue;
        std::string text = trim(rest);
        text = replace_all(text, "\\r\\n", "\r\n");
        text = replace_all(text, "\\n", "\n");
        parsed.emplace_back(num, std::move(text));
    }

    if (parsed.empty()) return {};

    std::vector<long long> nums;
    nums.reserve(parsed.size());
    for (const auto& [n, t] : parsed) nums.push_back(n);

    std::vector<long long> expected_1(expected_count), expected_0(expected_count);
    for (std::size_t i = 0; i < expected_count; ++i) {
        expected_1[i] = static_cast<long long>(i) + 1;
        expected_0[i] = static_cast<long long>(i);
    }

    if (nums == expected_1) {
        std::map<int, std::string> out;
        for (const auto& [n, t] : parsed) out[static_cast<int>(n)] = t;
        return out;
    }
    if (nums == expected_0) {
        log_warning("LLM 0-indexed its response (got " + std::to_string(nums.front()) + ".." +
                    std::to_string(nums.back()) + " for " + std::to_string(expected_count) +
                    " inputs); shifted to 1-indexed");
        std::map<int, std::string> out;
        for (const auto& [n, t] : parsed) out[static_cast<int>(n) + 1] = t;
        return out;
    }

    // Short lists in full; longer ones elided as first 6 ... last 3, so a
    // 150-line batch does not dump 150 numbers into the log.
    std::string nums_text;
    if (nums.size() <= 12) {
        nums_text = nums_repr(nums);
    } else {
        const std::vector<long long> head(nums.begin(), nums.begin() + 6);
        const std::vector<long long> tail(nums.end() - 3, nums.end());
        nums_text = nums_repr(head) + "..." + nums_repr(tail);
    }
    log_warning("LLM response malformed: " + std::to_string(parsed.size()) +
                " numbered lines for " + std::to_string(expected_count) + " inputs; nums=" +
                nums_text +
                ". Returning empty so the batch retries next run instead of poisoning the "
                "cache with misaligned pairs.");
    return {};
}

// --------------------------------------------------------------------------- //
// Per-file translation
// --------------------------------------------------------------------------- //

namespace {

// Returns (translated_count, stopped_early).
std::pair<long long, bool> translate_file(const bj::object& fdata, Cache& cache,
                                          int batch_size) {
    const std::vector<std::size_t> translatable = translatable_indices(fdata);
    if (translatable.empty()) return {0, false};

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
            if (cache.get(text)) {
                ++translated_count;
            } else {
                throw std::runtime_error(
                    "Offline build: missing cached translation; live API client removed for finished game");
            }
        }
    }

    return {translated_count, false};
}

}  // namespace

// --------------------------------------------------------------------------- //
// Driver
// --------------------------------------------------------------------------- //

int translate_all(const TranslateOptions& opt) {
    log_info("Batch size: " + std::to_string(opt.batch_size) + " lines per request");

    log_info("\nLoading extracted text...");
    bj::value all_data_v = json_parse_file(opt.input_file);
    bj::object& all_data = all_data_v.get_object();

    // Gate runs before any file access: wrong answer = zero cost.
    log_info("\n--- Speaker gate (no tokens spent) ---");
    const SpeakerCheckReport gate = run_speaker_checks(all_data);
    if (!gate.passed) {
        log_info("\n[ERROR] speaker check " + std::to_string(gate.failed_check) +
                 " failed: " + gate.reason);
        log_info("        Nothing was translated and nothing was written.");
        return 2;
    }

    // Refuse to discard the cache in an offline build.
    if (opt.fresh_run) {
        log_info("[ERROR] offline build: network API calls are disabled -- "
                 "refusing to discard the cache.");
        return 1;
    }

    log_info("Loading translation cache...");
    Cache cache = load_cache(opt.cache_file);
    if (opt.retranslate) {
        log_info("  --retranslate: starting fresh cache");
        cache = Cache{};
    } else {
        log_info("  Cache has " + std::to_string(cache.size()) + " entries");
        std::size_t purged = purge_failed_entries(cache);
        log_info("  Failed entries purged: " + std::to_string(purged) +
                 (purged ? "  (those lines re-queue)" : ""));
    }

    // Glossary pre-population; re-setting an existing key does not move it.
    for (const auto& [jp, en] : name_translations_ordered()) cache.set(jp, en);

    std::vector<std::string> file_order;
    for (const auto& f : story_order())
        if (all_data.if_contains(f)) file_order.push_back(f);
    for (const auto& kv : all_data) {
        const std::string name(kv.key());
        if (std::find(file_order.begin(), file_order.end(), name) == file_order.end())
            file_order.push_back(name);
    }

    // (fname, translatable, uncached)
    std::vector<std::tuple<std::string, std::size_t, std::size_t>> file_stats;
    std::size_t total_translatable = 0, total_uncached = 0;
    for (const auto& fname : file_order) {
        if (opt.only_file && fname != *opt.only_file) continue;
        const auto& fdata = all_data.at(fname).get_object();
        const auto idxs = translatable_indices(fdata);
        const auto& strings = fdata.at("strings").get_array();
        std::size_t uncached = 0;
        for (std::size_t i : idxs)
            if (!cache.contains(json_str(strings[i].get_object(), "text"))) ++uncached;
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
        // Don't rewrite the cache file just for glossary pre-population.
        log_info("  Everything is already cached!");
    } else {
        log_info("\n--- Translating in story order ---");
        std::size_t done_lines = 0;
        auto start = std::chrono::steady_clock::now();

        for (const auto& [fname, line_count, uncached_count] : file_stats) {
            if (uncached_count == 0) {
                done_lines += line_count;
                continue;
            }

            log_info("\n  [" + fname + "] " + std::to_string(uncached_count) + " uncached / " +
                     std::to_string(line_count) + " translatable");

            auto [count, stopped] =
                translate_file(all_data.at(fname).get_object(), cache, opt.batch_size);
            (void)count;
            done_lines += line_count;

            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            const double rate = elapsed > 0 ? static_cast<double>(done_lines) / elapsed : 0.0;
            char buf[160];
            std::snprintf(buf, sizeof(buf), "  Progress: %zu/%zu (%zu%%) | %.1f l/s", done_lines,
                          total_translatable,
                          total_translatable ? done_lines * 100 / total_translatable : 0, rate);
            log_info(buf);

            if (stopped) {
                log_info("\n  " + std::string(opt.test_mode ? "--test " : "--max-batches ") +
                         std::to_string(opt.test_batches) + ": stopping after " +
                         std::to_string(opt.test_batches) + " batch(es)");
                break;
            }
        }

        save_cache(cache, opt.cache_file);
        log_info("\nTranslation done! Cache: " + std::to_string(cache.size()) + " entries");
    }

    log_info("\nBuilding translated text...");
    for (auto& kv : all_data) {
        auto* strings = kv.value().get_object().if_contains("strings");
        if (!strings || !strings->is_array()) continue;
        for (auto& sv : strings->get_array()) {
            auto& s = sv.get_object();
            const std::string text = json_str(s, "text");
            const std::string* en = cache.get(text);
            s["translated"] = (en && *en != text) ? *en : text;
        }
    }

    write_file_text(opt.output_file, json_pretty(all_data_v, 1));
    char sz[64];
    std::snprintf(sz, sizeof(sz), "%.1f MB",
                  static_cast<double>(fs::file_size(fs::u8path(opt.output_file))) / 1024.0 /
                      1024.0);
    log_info("Saved to " + opt.output_file + " (" + sz + ")");

    log_info("\n--- Building the runtime translation table ---");
    return build_tsv::run_build(opt.output_file, opt.tsv_file);
}

}  // namespace frat::translate
