// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "translate/translate_core.h"

#include "build_tsv/build_tsv.h"
#include "common/util.h"
#include "translate/glossary.h"

#include <boost/regex.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace adi::translate {

namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

constexpr char32_t kHiraganaFirst = 0x3040;
constexpr char32_t kHiraganaLast = 0x309F;
constexpr char32_t kKatakanaFirst = 0x30A0;
constexpr char32_t kKatakanaLast = 0x30FF;
constexpr char32_t kCjkUnifiedFirst = 0x4E00;
constexpr char32_t kCjkUnifiedLast = 0x9FFF;
constexpr char32_t kCjkExtAFirst = 0x3400;
constexpr char32_t kCjkExtALast = 0x4DBF;
constexpr std::size_t kShortNonJpCodepointLimit = 2;
constexpr unsigned char kNonAsciiLeadMin = 0x80;
constexpr int kCacheJsonIndent = 2;
constexpr int kOutputJsonIndent = 1;
constexpr std::size_t kContextLineCharLimit = 120;
constexpr std::size_t kBatchPreviewCharLimit = 60;
constexpr std::int64_t kTokensPerBatchLine = 300;
constexpr std::int64_t kMinMaxOutputTokens = 16384;
constexpr int kRateLimitRetrySeconds = 60;
constexpr std::size_t kContextTrimMultiplier = 2;
constexpr std::size_t kPercentScale = 100;
constexpr std::size_t kProgressBufLen = 160;
constexpr std::size_t kSizeBufLen = 64;
constexpr double kBytesPerMegabyte = 1024.0 * 1024.0;
constexpr int kExitError = 1;

struct UniqueBatchItem {
    std::string speaker;
    std::string text;
};

struct WindowEntry {
    std::size_t str_idx = 0;
    std::string type;
    std::string speaker;
    std::string text;
    bool was_uncached = false;
};

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

std::string obj_str(const bj::object& o, const char* key) {
    if (auto* v = o.if_contains(key)) {
        if (v->is_string()) {
            return std::string(v->get_string());
        }
    }
    return {};
}

void print_context_window(
    const std::vector<std::pair<std::string, std::string>>& previous_context) {
    const std::size_t n = std::min<std::size_t>(
        previous_context.size(), static_cast<std::size_t>(CONTEXT_WINDOW));
    log_info("\n=== ROLLING CONTEXT WINDOW (last " + std::to_string(n) + " of " +
             std::to_string(previous_context.size()) + ") ===");
    if (n == 0) {
        log_info("  (empty -- start of scene)");
        log_info("=== END CONTEXT ===\n");
        return;
    }
    for (std::size_t i = previous_context.size() - n; i < previous_context.size(); ++i) {
        log_info("  [" + previous_context[i].first + "] " +
                 flatten_for_log(previous_context[i].second, kContextLineCharLimit));
    }
    log_info("=== END CONTEXT ===\n");
}

void write_last_translation_snapshot(
    const TranslateOptions& opt,
    const std::string& scene_name,
    int api_batches_done,
    const std::vector<UniqueBatchItem>& unique_items,
    const Cache& cache) {
    fs::path base_dir = fs::u8path(opt.cache_file).parent_path();
    if (base_dir.empty()) {
        base_dir = fs::u8path(opt.output_file).parent_path();
    }
    if (base_dir.empty()) {
        return;
    }

    const fs::path snap_dir = base_dir / "last_gemini_translate";
    fs::create_directories(snap_dir);
    const fs::path snap_path = snap_dir / "last_translation.json";

    bj::array items_arr;
    for (const auto& u : unique_items) {
        bj::object row;
        row["jp"] = u.text;
        row["speaker"] = u.speaker;
        const std::string* en = cache.get(u.text);
        row["en"] = en ? *en : u.text;
        items_arr.push_back(std::move(row));
    }

    bj::object snap;
    snap["scene"] = scene_name;
    snap["api_batches_done"] = static_cast<std::int64_t>(api_batches_done);
    snap["items"] = std::move(items_arr);

    write_file_atomic_text(
        snap_path.u8string(),
        json_pretty(bj::value(std::move(snap)), kCacheJsonIndent));
}

bool translate_scene_impl(const std::string& scene_name,
                          const bj::object& scene_data,
                          Cache& cache,
                          const TranslateOptions& opt,
                          gemini::IMessageClient& client,
                          int& api_batches_done,
                          bj::array* log_entries) {
    const std::vector<std::size_t> translatable =
        translatable_indices(scene_data, opt.skip_to_dialogue);
    if (translatable.empty()) {
        return false;
    }

    const auto& strings = scene_data.at("strings").get_array();
    std::vector<std::pair<std::string, std::string>> previous_context;
    const std::size_t max_unique_per_batch = static_cast<std::size_t>(
        std::max(1, opt.batch_size));

    std::size_t cursor = 0;
    while (cursor < translatable.size()) {
        // Advance past all contiguous already-cached prefix lines, feeding them
        // directly into the rolling context window with zero API calls.
        while (cursor < translatable.size()) {
            const std::size_t idx = translatable[cursor];
            const auto& s = strings[idx].get_object();
            const std::string text(s.at("text").get_string());
            const std::string* en = cache.get(text);
            if (!en) {
                break;
            }
            previous_context.emplace_back(extract_speaker(s), *en);
            ++cursor;
        }
        if (cursor >= translatable.size()) {
            break;
        }
        if (opt.max_batches > 0 && api_batches_done >= opt.max_batches) {
            return true;
        }

        // Scan forward to pack up to `batch_size` unique uncached Japanese lines
        // into one API call, deduplicating identical Japanese strings within the
        // window while retaining interleaved cached lines for ordered replay.
        std::vector<UniqueBatchItem> unique_uncached;
        unique_uncached.reserve(max_unique_per_batch);
        std::set<std::string> seen_in_batch;
        std::vector<WindowEntry> window_entries;

        while (cursor < translatable.size() &&
               unique_uncached.size() < max_unique_per_batch) {
            const std::size_t idx = translatable[cursor];
            const auto& s = strings[idx].get_object();
            const std::string text(s.at("text").get_string());
            const std::string speaker = extract_speaker(s);
            const std::string type = obj_str(s, "type");

            if (cache.contains(text)) {
                window_entries.push_back(WindowEntry{idx, type, speaker, text, false});
                ++cursor;
                continue;
            }
            if (seen_in_batch.insert(text).second) {
                unique_uncached.push_back(UniqueBatchItem{speaker, text});
            }
            window_entries.push_back(WindowEntry{idx, type, speaker, text, true});
            ++cursor;
        }

        // Consume any trailing immediate duplicates of lines already in `seen_in_batch`
        // before the next genuinely new uncached string, so duplicate repeats never
        // spill into a second API call.
        while (cursor < translatable.size()) {
            const std::size_t idx = translatable[cursor];
            const auto& s = strings[idx].get_object();
            const std::string text(s.at("text").get_string());
            if (!seen_in_batch.count(text)) {
                break;
            }
            window_entries.push_back(
                WindowEntry{idx, obj_str(s, "type"), extract_speaker(s), text, true});
            ++cursor;
        }

        ++api_batches_done;
        log_info("\n--- " + scene_name + " API batch #" +
                 std::to_string(api_batches_done) + " (" +
                 std::to_string(unique_uncached.size()) + " unique uncached lines) ---");
        print_context_window(previous_context);

        std::vector<std::pair<std::string, std::string>> api_lines;
        api_lines.reserve(unique_uncached.size());
        for (const auto& u : unique_uncached) {
            api_lines.emplace_back(u.speaker, u.text);
        }

        auto store_translations = [&](const std::map<int, std::string>& translations,
                                      bool verbose) {
            for (std::size_t i = 0; i < unique_uncached.size(); ++i) {
                const auto& u = unique_uncached[i];
                auto it = translations.find(static_cast<int>(i) + 1);
                if (it == translations.end() || it->second.empty()) {
                    if (verbose) {
                        log_info("    Missing translation for line " +
                                 std::to_string(i + 1) + ", keeping original");
                    }
                    continue;
                }
                const std::string tr = postprocess(u.text, it->second);
                cache.set(u.text, tr);
                if (verbose) {
                    log_info("  JP: " + flatten_for_log(u.text, kBatchPreviewCharLimit));
                    log_info("  EN: " + flatten_for_log(tr, kBatchPreviewCharLimit));
                }
            }
        };

        try {
            auto translations = call_gemini(client, api_lines, previous_context);
            if (translations.size() < unique_uncached.size()) {
                log_info("    WARNING: got " + std::to_string(translations.size()) + "/" +
                         std::to_string(unique_uncached.size()) + " translations");
            }
            store_translations(translations, /*verbose=*/true);
        } catch (const gemini::RateLimitError&) {
            log_info("    Rate limited, waiting 60s...");
            std::this_thread::sleep_for(std::chrono::seconds(kRateLimitRetrySeconds));
            try {
                store_translations(
                    call_gemini(client, api_lines, previous_context), /*verbose=*/false);
            } catch (const std::exception& e2) {
                log_info(std::string("    Retry failed: ") + e2.what() +
                         ", keeping originals");
            }
        } catch (const std::exception& e) {
            log_info(std::string("    Batch error: ") + e.what());
        }

        for (const auto& w : window_entries) {
            const std::string* en = cache.get(w.text);
            const std::string resolved = en ? *en : w.text;
            previous_context.emplace_back(w.speaker, resolved);
            if (w.was_uncached && en && log_entries) {
                bj::object e;
                e["scene"] = scene_name;
                e["str_idx"] = static_cast<std::int64_t>(w.str_idx);
                e["type"] = w.type;
                e["speaker"] = w.speaker;
                e["japanese"] = w.text;
                e["translated"] = *en;
                log_entries->push_back(std::move(e));
            }
        }

        const std::size_t max_retained_context =
            static_cast<std::size_t>(CONTEXT_WINDOW) * kContextTrimMultiplier;
        if (previous_context.size() > max_retained_context) {
            previous_context.erase(
                previous_context.begin(),
                previous_context.end() -
                    static_cast<std::ptrdiff_t>(max_retained_context));
        }

        if (!opt.cache_file.empty()) {
            save_cache(cache, opt.cache_file);
        }
        write_last_translation_snapshot(
            opt, scene_name, api_batches_done, unique_uncached, cache);

        if (opt.max_batches > 0 && api_batches_done >= opt.max_batches) {
            // Check whether any uncached lines remain in this scene after `cursor`.
            for (std::size_t rem = cursor; rem < translatable.size(); ++rem) {
                const auto& rs = strings[translatable[rem]].get_object();
                if (!cache.contains(std::string(rs.at("text").get_string()))) {
                    return true;
                }
            }
            return true;
        }
    }

    return false;
}

}  // namespace

std::optional<std::string> validate_options(int batch_size, int test_n, int max_batches) {
    if (batch_size < 1) {
        return "--batch must be at least 1 (got " + std::to_string(batch_size) + ")";
    }
    if (test_n < 0) {
        return "--test cannot be negative (got " + std::to_string(test_n) + ")";
    }
    if (max_batches < 0) {
        return "--max-batches cannot be negative (got " + std::to_string(max_batches) +
               "); use 0 for no limit";
    }
    return std::nullopt;
}

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
    if (char_len(text) <= kShortNonJpCodepointLimit && !has_real_japanese(text)) {
        return false;
    }
    return has_real_japanese(text);
}

std::vector<std::size_t> translatable_indices(const bj::object& scene_data,
                                              bool skip_to_dialogue) {
    std::vector<std::size_t> out;
    const auto* strings = scene_data.if_contains("strings");
    if (!strings || !strings->is_array()) {
        return out;
    }
    const auto& arr = strings->get_array();
    for (std::size_t i = 0; i < arr.size(); ++i) {
        const auto& s = arr[i].get_object();
        const std::string text = obj_str(s, "text");
        const std::string type = obj_str(s, "type");
        if (needs_translation(text) &&
            (type == "dialogue" || type == "narrative" || type == "name" || type == "menu")) {
            out.push_back(i);
        }
    }
    if (skip_to_dialogue) {
        for (std::size_t j = 0; j < out.size(); ++j) {
            const std::string text = obj_str(arr[out[j]].get_object(), "text");
            if (text.find("「") != std::string::npos) {
                return std::vector<std::size_t>(
                    out.begin() + static_cast<std::ptrdiff_t>(j), out.end());
            }
        }
    }
    return out;
}

std::optional<std::string> match_speaker_name(const std::string& jp_name) {
    const auto& map = name_translations();
    auto exact = map.find(jp_name);
    if (exact != map.end()) {
        return exact->second;
    }

    const std::string* best_jp = nullptr;
    const std::string* best_en = nullptr;
    for (const auto& [known_jp, known_en] : name_translations_ordered()) {
        if (known_jp.empty()) {
            continue;
        }
        if (jp_name.size() < known_jp.size() ||
            jp_name.compare(jp_name.size() - known_jp.size(), known_jp.size(), known_jp) != 0) {
            continue;
        }
        if (!best_jp || char_len(known_jp) > char_len(*best_jp)) {
            best_jp = &known_jp;
            best_en = &known_en;
        }
    }
    if (best_en) {
        return *best_en;
    }
    return std::nullopt;
}

std::string extract_speaker(const bj::object& s) {
    if (obj_str(s, "type") == "name") {
        return "NAME";
    }
    const std::string speaker = obj_str(s, "speaker");
    if (!speaker.empty()) {
        const auto en = match_speaker_name(speaker);
        return (en && !en->empty()) ? *en : speaker;
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
    return sanitize_ascii(translated);
}

std::string flatten_for_log(const std::string& txt, std::size_t limit) {
    std::string flat = replace_all(txt, "\r\n", "|");
    flat = replace_all(flat, "\n", "|");
    std::string out;
    std::size_t i = 0;
    std::size_t n = 0;
    while (i < flat.size() && n < limit) {
        const std::size_t start = i;
        utf8_next(flat, i);
        out.append(flat, start, i - start);
        ++n;
    }
    return out;
}

Cache load_cache(const std::string& cache_file) {
    Cache cache;
    if (!fs::exists(fs::u8path(cache_file))) {
        return cache;
    }
    bj::value root = json_parse_file(cache_file);
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

void save_cache(const Cache& cache, const std::string& cache_file) {
    const fs::path parent = fs::u8path(cache_file).parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent);
    }
    bj::object o;
    for (const auto& [jp, en] : cache.items()) {
        o[jp] = en;
    }
    write_file_atomic_text(cache_file, json_pretty(bj::value(std::move(o)), kCacheJsonIndent));
}

void seed_default_glossary(Cache& cache) {
    for (const auto& [jp, en] : name_translations_ordered()) {
        if (!cache.contains(jp)) {
            cache.set(jp, en);
        }
    }
    for (const auto& [jp, en] : default_seed_translations()) {
        if (!cache.contains(jp)) {
            cache.set(jp, en);
        }
    }
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
           "        To cap a run and keep them, use --max-batches N instead.\n"
           "        To delete them anyway, add --discard-cache.";
}

bool is_failed_entry(const std::string& jp, const std::string& en) {
    if (jp != en) {
        return false;
    }
    for (char c : jp) {
        if (static_cast<unsigned char>(c) >= kNonAsciiLeadMin) {
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
    if (removed > 0) {
        cache = std::move(kept);
    }
    return removed;
}

std::string build_user_prompt(
    const std::vector<std::pair<std::string, std::string>>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context) {
    std::string user_prompt;
    if (!previous_context.empty()) {
        user_prompt += "<previous_context>\n";
        const std::size_t window = static_cast<std::size_t>(CONTEXT_WINDOW);
        const std::size_t start =
            previous_context.size() > window ? previous_context.size() - window : 0;
        for (std::size_t i = start; i < previous_context.size(); ++i) {
            user_prompt += "[" + previous_context[i].first + "] " +
                           flatten_for_log(previous_context[i].second, kContextLineCharLimit) +
                           "\n";
        }
        user_prompt += "</previous_context>\n\n";
    }
    user_prompt += "<lines_to_translate>\n";
    for (std::size_t i = 0; i < batch_lines.size(); ++i) {
        std::string escaped = replace_all(batch_lines[i].second, "\r\n", "\\r\\n");
        escaped = replace_all(escaped, "\n", "\\n");
        user_prompt +=
            std::to_string(i + 1) + ". [" + batch_lines[i].first + "] " + escaped + "\n";
    }
    user_prompt += "</lines_to_translate>\n\n";
    user_prompt +=
        "Translate the " + std::to_string(batch_lines.size()) +
        " lines inside <lines_to_translate> to English. "
        "Use the <previous_context> (if present) for scene continuity but do NOT translate it. "
        "Output EXACTLY " + std::to_string(batch_lines.size()) +
        " numbered translations, one per line. "
        "Do NOT include [SPEAKER] tags in your output.";
    return user_prompt;
}

std::map<int, std::string> parse_numbered_response(const std::string& response_text) {
    static const boost::regex tag_re(R"RE(^\[(?:NARRATION|NAME|[A-Za-z\s()'?]+)\]\s*)RE");
    static const boost::regex num_re(R"RE(^(\d+)[.):\s]+\s*(.*))RE");
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
        line = boost::regex_replace(line, tag_re, "");
        boost::smatch m;
        if (boost::regex_match(line, m, num_re)) {
            std::string text = trim(m[2].str());
            text = replace_all(text, "\\r\\n", "\r\n");
            text = replace_all(text, "\\n", "\n");
            translations[std::stoi(m[1].str())] = text;
        }
    }
    return translations;
}

std::map<int, std::string> call_gemini(
    gemini::IMessageClient& client,
    const std::vector<std::pair<std::string, std::string>>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context) {
    bj::object body;
    body["model"] = MODEL;
    body["max_tokens"] = std::max<std::int64_t>(
        static_cast<std::int64_t>(batch_lines.size()) * kTokensPerBatchLine,
        kMinMaxOutputTokens);
    body["system"] = SYSTEM_PROMPT;
    bj::object msg;
    msg["role"] = "user";
    msg["content"] = build_user_prompt(batch_lines, previous_context);
    body["messages"] = bj::array{std::move(msg)};

    bj::value response = client.messages(bj::value(std::move(body)));

    std::string response_text;
    if (auto* content = response.get_object().if_contains("content")) {
        for (const auto& block : content->get_array()) {
            if (auto* t = block.get_object().if_contains("type")) {
                if (t->is_string() && t->get_string() == "text") {
                    if (auto* txt = block.get_object().if_contains("text")) {
                        if (txt->is_string()) {
                            response_text = trim(std::string(txt->get_string()));
                        }
                    }
                    break;
                }
            }
        }
    }

    log_info("\n--- GEMINI'S RAW ANSWER ---");
    log_info(response_text);
    log_info("---------------------------\n");

    std::int64_t in_tok = 0;
    std::int64_t out_tok = 0;
    std::int64_t cache_read = 0;
    std::int64_t cache_create = 0;
    if (auto* usage = response.get_object().if_contains("usage")) {
        const auto& u = usage->get_object();
        auto geti = [&](const char* k) -> std::int64_t {
            if (auto* v = u.if_contains(k)) {
                if (v->is_int64()) {
                    return v->get_int64();
                }
            }
            return 0;
        };
        in_tok = geti("input_tokens");
        out_tok = geti("output_tokens");
        cache_read = geti("cache_read_input_tokens");
        cache_create = geti("cache_creation_input_tokens");
    }
    log_info("    Tokens: in=" + std::to_string(in_tok) + " out=" + std::to_string(out_tok) +
             " cache_read=" + std::to_string(cache_read) +
             " cache_create=" + std::to_string(cache_create));

    return parse_numbered_response(response_text);
}

bool translate_scene(const std::string& scene_name,
                     const bj::object& scene_data,
                     Cache& cache,
                     const TranslateOptions& opt,
                     gemini::IMessageClient& client,
                     int& api_batches_done) {
    return translate_scene_impl(
        scene_name, scene_data, cache, opt, client, api_batches_done, nullptr);
}

int translate_all(const TranslateOptions& opt) {
    if (opt.fresh_run && gemini::load_api_key().empty()) {
        log_info("[ERROR] GEMINI_API_KEY not set -- refusing to discard the "
                 "cache for a run that cannot translate.");
        return kExitError;
    }
    gemini::LazyClient client;
    return translate_all(opt, client);
}

int translate_all(const TranslateOptions& opt, gemini::IMessageClient& client) {
    log_info("Model: " + MODEL);
    if (is_effort_model(MODEL)) {
        log_info("Effort: " + EFFORT);
    }
    log_info("Batch size: " + std::to_string(opt.batch_size) + " lines per request");
    log_info("Context window: " + std::to_string(CONTEXT_WINDOW) + " previous lines");

    if (opt.fresh_run) {
        for (const std::string& p : {opt.cache_file, opt.output_file}) {
            const fs::path path = fs::u8path(p);
            std::error_code ec;
            const bool present = fs::exists(path, ec);
            if (!ec && !present) {
                continue;
            }
            const bool removed = !ec && fs::remove(path, ec);
            if (ec) {
                log_info("[ERROR] cannot delete " + p + ": " + ec.message() +
                         " -- close whatever has it open, then retry.");
                return kExitError;
            }
            log_info(removed ? "Fresh run: removed " + p
                             : "Fresh run: " + p + " was already gone");
        }
    }

    log_info("\nLoading extracted text...");
    bj::value all_data_v = json_parse_file(opt.input_file);
    bj::object& all_data = all_data_v.get_object();

    log_info("Loading translation cache...");
    Cache cache = opt.retranslate ? Cache{} : load_cache(opt.cache_file);
    if (opt.retranslate) {
        log_info("  --retranslate: starting fresh cache");
    }
    if (!opt.retranslate) {
        log_info("  Cache has " + std::to_string(cache.size()) + " entries");
        const std::size_t purged = purge_failed_entries(cache);
        log_info("  Failed entries purged: " + std::to_string(purged) +
                 (purged ? "  (those lines re-queue)" : ""));
    }

    seed_default_glossary(cache);

    std::vector<std::string> scene_order;
    std::set<std::string> placed;
    for (const auto& s : story_order()) {
        if (all_data.if_contains(s) && placed.insert(s).second) {
            scene_order.push_back(s);
        }
    }
    std::vector<std::string> remainder;
    for (const auto& kv : all_data) {
        const std::string name(kv.key());
        if (!placed.count(name)) {
            remainder.push_back(name);
        }
    }
    std::sort(remainder.begin(), remainder.end());
    scene_order.insert(scene_order.end(), remainder.begin(), remainder.end());

    std::vector<std::tuple<std::string, std::size_t, std::size_t>> scene_stats;
    std::size_t total_translatable = 0;
    std::size_t total_uncached = 0;
    for (const auto& sname : scene_order) {
        if (opt.only_scene && sname != *opt.only_scene) {
            continue;
        }
        const auto& sdata = all_data.at(sname).get_object();
        const auto idxs = translatable_indices(sdata, opt.skip_to_dialogue);
        const auto& strings = sdata.at("strings").get_array();
        std::size_t uncached = 0;
        for (std::size_t i : idxs) {
            if (!cache.contains(
                    std::string(strings[i].get_object().at("text").get_string()))) {
                ++uncached;
            }
        }
        scene_stats.emplace_back(sname, idxs.size(), uncached);
        total_translatable += idxs.size();
        total_uncached += uncached;
    }

    log_info("\nTranslation summary:");
    log_info("  Scenes to process: " + std::to_string(scene_stats.size()));
    log_info("  Total translatable lines: " + std::to_string(total_translatable));
    log_info("  Need translating: " + std::to_string(total_uncached) +
             " (cached: " + std::to_string(total_translatable - total_uncached) + ")");

    if (total_uncached == 0) {
        log_info("  Everything is already cached!");
        save_cache(cache, opt.cache_file);
    }
    if (total_uncached > 0) {
        log_info("\n--- Translating in chronological story order ---");
        std::size_t done_lines = 0;
        int api_batches_done = 0;
        auto start = std::chrono::steady_clock::now();

        for (const auto& [sname, line_count, uncached_count] : scene_stats) {
            if (uncached_count == 0) {
                done_lines += line_count;
                continue;
            }
            if (opt.max_batches > 0 && api_batches_done >= opt.max_batches) {
                log_info("\n  batch cap reached: stopping after " +
                         std::to_string(api_batches_done) + " batch(es)");
                break;
            }

            log_info("\n  [" + sname + "] " + std::to_string(uncached_count) +
                     " uncached / " + std::to_string(line_count) + " translatable");

            bj::array log_entries;
            const bool stopped = translate_scene_impl(
                sname,
                all_data.at(sname).get_object(),
                cache,
                opt,
                client,
                api_batches_done,
                opt.fresh_run ? &log_entries : nullptr);
            done_lines += line_count;

            if (opt.fresh_run && !log_entries.empty()) {
                fs::create_directories(fs::u8path(opt.test_dir));
                int run_num = 1;
                while (fs::exists(fs::u8path(opt.test_dir + "\\translation_run_" +
                                             std::to_string(run_num) + ".json"))) {
                    ++run_num;
                }
                const std::string log_path =
                    opt.test_dir + "\\translation_run_" + std::to_string(run_num) + ".json";
                write_file_text(log_path,
                                json_pretty(bj::value(log_entries), kCacheJsonIndent));
                log_info("  Test log: " + log_path + " (" +
                         std::to_string(log_entries.size()) + " entries)");
            }

            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
                    .count();
            const double rate =
                elapsed > 0 ? static_cast<double>(done_lines) / elapsed : 0.0;
            char buf[kProgressBufLen];
            std::snprintf(buf, sizeof(buf), "  Progress: %zu/%zu (%zu%%) | %.1f l/s",
                          done_lines, total_translatable,
                          done_lines * kPercentScale /
                              (total_translatable ? total_translatable : 1),
                          rate);
            log_info(buf);

            if (stopped || (opt.max_batches > 0 && api_batches_done >= opt.max_batches)) {
                log_info("\n  batch cap reached: stopping after " +
                         std::to_string(api_batches_done) + " batch(es)");
                break;
            }
        }

        save_cache(cache, opt.cache_file);
        log_info("\nTranslation done! Cache: " + std::to_string(cache.size()) +
                 " entries");
    }

    log_info("\nBuilding translated text...");
    for (auto& kv : all_data) {
        auto* strings = kv.value().get_object().if_contains("strings");
        if (!strings || !strings->is_array()) {
            continue;
        }
        for (auto& sv : strings->get_array()) {
            auto& s = sv.get_object();
            const std::string text = obj_str(s, "text");
            const std::string* en = cache.get(text);
            s["translated"] = (en && *en != text) ? postprocess(text, *en) : text;
        }
    }

    const fs::path out_parent = fs::u8path(opt.output_file).parent_path();
    if (!out_parent.empty()) {
        fs::create_directories(out_parent);
    }
    write_file_text(opt.output_file, json_pretty(all_data_v, kOutputJsonIndent));
    char sz[kSizeBufLen];
    std::snprintf(sz, sizeof(sz), "%.1f MB",
                  static_cast<double>(fs::file_size(fs::u8path(opt.output_file))) /
                      kBytesPerMegabyte);
    log_info("Saved to " + opt.output_file + " (" + sz + ")");

    log_info("\n--- Building the runtime translation table ---");
    return build_tsv::run_build(opt.output_file, opt.tsv_file);
}

}  // namespace adi::translate
