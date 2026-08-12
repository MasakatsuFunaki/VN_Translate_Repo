// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "translate_core.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

#include <boost/regex.hpp>

#include "common/util.h"
#include "glossary.h"
#include "speaker_gate.h"

namespace exc::translate {

namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

std::size_t char_len(const std::string& utf8) {
    std::size_t n = 0, i = 0;
    while (i < utf8.size()) {
        utf8_next(utf8, i);
        ++n;
    }
    return n;
}

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

}  // namespace

std::optional<std::string> validate_options(int batch_size, int test_batches) {
    if (batch_size <= 0)
        return "--batch must be at least 1 (got " + std::to_string(batch_size) + ")";
    if (test_batches < 0)
        return "--test must not be negative (got " + std::to_string(test_batches) + ")";
    return std::nullopt;
}

std::string strip(const std::string& s) { return trim(s); }

bool has_real_japanese(const std::string& text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const char32_t cp = utf8_next(text, i);
        if ((cp >= 0x3040 && cp <= 0x309F) || (cp >= 0x30A0 && cp <= 0x30FF) ||
            (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF))
            return true;
    }
    return false;
}

bool needs_translation(const std::string& text) {
    if (text.empty() || trim(text).empty()) return false;
    if (char_len(text) <= 3 && !has_real_japanese(text)) return false;
    return has_real_japanese(text);
}

std::vector<std::size_t> translatable_indices(const bj::object& fdata) {
    std::vector<std::size_t> out;
    const auto* strings = fdata.if_contains("strings");
    if (!strings || !strings->is_array()) return out;
    const auto& arr = strings->get_array();
    for (std::size_t i = 0; i < arr.size(); ++i) {
        const auto& s = arr[i].get_object();
        std::string text, type;
        if (auto* t = s.if_contains("text"))
            if (t->is_string()) text = std::string(t->get_string());
        if (auto* t = s.if_contains("type"))
            if (t->is_string()) type = std::string(t->get_string());
        if (needs_translation(text) &&
            (type == "dialogue" || type == "narrative" || type == "menu"))
            out.push_back(i);
    }
    return out;
}

std::optional<std::string> match_speaker_name(const std::string& jp_name) {
    const auto& map = name_translations();
    auto it = map.find(jp_name);
    if (it != map.end()) return it->second;

    // Suffix match: the longest known name this one ends with.  Iterating the
    // ORDERED table (not the hash map) keeps tie-breaks deterministic -- the
    // hash map's order varies and would make two runs disagree.
    const std::string* best_jp = nullptr;
    const std::string* best_en = nullptr;
    for (const auto& [known_jp, known_en] : name_translations_ordered()) {
        if (known_jp.empty()) continue;
        if (jp_name.size() < known_jp.size()) continue;
        if (jp_name.compare(jp_name.size() - known_jp.size(), known_jp.size(), known_jp) != 0)
            continue;
        if (!best_jp || char_len(known_jp) > char_len(*best_jp)) {
            best_jp = &known_jp;
            best_en = &known_en;
        }
    }
    if (best_en) return *best_en;
    return std::nullopt;
}

std::string extract_speaker(const std::string& text) {
    // Real dialogue always opens with a Japanese quote bracket on the line
    // after the speaker name.  Without that gate, single-line prose entries
    // that merely happen to be typed 'dialogue' get their leading prose
    // mis-tagged as the speaker.
    const std::size_t crlf = text.find("\r\n");
    if (crlf == std::string::npos) return "NARRATION";
    const std::string name = trim(text.substr(0, crlf));
    const std::string rest = trim_left(text.substr(crlf + 2));
    if (name.empty() || char_len(name) > 20 || rest.empty()) return "NARRATION";
    if (rest.rfind("\xE3\x80\x8C", 0) != 0 &&   // 「
        rest.rfind("\xE3\x80\x8E", 0) != 0)     // 『
        return "NARRATION";
    auto en = match_speaker_name(name);
    return en ? *en : name;
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

std::string postprocess(const std::string& original, const std::string& translated) {
    if (translated.empty() || translated == original) return translated;
    std::string t = sanitize_ascii(translated);

    // Re-attach the speaker name for dialogue.  The engine renders the text
    // before the first \r\n as the name plate, so attaching the English name
    // makes the plate read "Miyaka" rather than 美弥香.  Only treat the first
    // segment as a name when it is short (<=10 chars), so a narrative line
    // break is never mistaken for a speaker prefix.
    const std::size_t crlf = original.find("\r\n");
    if (crlf != std::string::npos) {
        const std::string jp_name = trim(original.substr(0, crlf));
        if (!jp_name.empty() && char_len(jp_name) <= 10) {
            // Suffix-tolerant, so a name carrying extraction control bytes
            // still resolves.
            if (auto en = match_speaker_name(jp_name)) {
                const std::string& en_name = *en;
                if (t.rfind(en_name + "\r\n", 0) != 0) {
                    if (t.rfind(jp_name + "\r\n", 0) == 0)
                        t = t.substr(jp_name.size() + 2);
                    t = en_name + "\r\n" + t;
                }
            }
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
    for (const auto& kv : root.get_object())
        if (kv.value().is_string())
            cache.set(std::string(kv.key()), std::string(kv.value().get_string()));
    return cache;
}

void save_cache(const Cache& cache, const std::string& cache_file) {
    bj::object o;
    for (const auto& [jp, en] : cache.items()) o[jp] = en;
    write_file_atomic_text(cache_file, json_pretty(bj::value(std::move(o)), 2));
}

// --------------------------------------------------------------------------- //
// API call
// --------------------------------------------------------------------------- //

namespace {

// "a\r\nb" -> "a|b", truncated to `limit` CODEPOINTS.
//
// Must not be a byte truncation: the 100-limit form below feeds
// build_user_prompt, i.e. the actual request body.  Cutting by bytes would
// both shorten the context Claude sees and risk splitting a multi-byte
// sequence -- boost::json does not validate UTF-8, so a malformed body would
// reach the API and cost the whole batch.
std::string flatten_for_log(const std::string& txt, std::size_t limit) {
    const std::string flat = replace_all(txt, "\r\n", "|");
    std::string out;
    std::size_t i = 0, n = 0;
    while (i < flat.size() && n < limit) {
        const std::size_t start = i;
        utf8_next(flat, i);
        out.append(flat, start, i - start);
        ++n;
    }
    return out;
}

void print_context_window(
    const std::vector<std::pair<std::string, std::string>>& previous_context) {
    const std::size_t n = std::min<std::size_t>(previous_context.size(), CONTEXT_WINDOW);
    log_info("\n=== ROLLING CONTEXT WINDOW (last " + std::to_string(n) + " of " +
             std::to_string(previous_context.size()) + ") ===");
    if (n == 0) {
        log_info("  (empty -- start of file)");
    } else {
        for (std::size_t i = previous_context.size() - n; i < previous_context.size(); ++i)
            log_info("  [" + previous_context[i].first + "] " +
                     flatten_for_log(previous_context[i].second, 120));
    }
    log_info("=== END CONTEXT ===\n");
}

}  // namespace

std::string build_user_prompt(
    const std::vector<std::pair<std::string, std::string>>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context) {
    std::string user_prompt;
    if (!previous_context.empty()) {
        user_prompt += "<previous_context>\n";
        const std::size_t start = previous_context.size() > CONTEXT_WINDOW
                                      ? previous_context.size() - CONTEXT_WINDOW
                                      : 0;
        for (std::size_t i = start; i < previous_context.size(); ++i)
            user_prompt += "[" + previous_context[i].first + "] " +
                           flatten_for_log(previous_context[i].second, 100) + "\n";
        user_prompt += "</previous_context>\n\n";
    }
    user_prompt += "<lines_to_translate>\n";
    for (std::size_t i = 0; i < batch_lines.size(); ++i) {
        std::string escaped = replace_all(batch_lines[i].second, "\r\n", "\\r\\n");
        escaped = replace_all(escaped, "\n", "\\n");
        user_prompt += std::to_string(i + 1) + ". [" + batch_lines[i].first + "] " + escaped + "\n";
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
    static const boost::regex tag_re(R"(^\[(?:NARRATION|[A-Za-z\s()'?]+)\]\s*)");
    static const boost::regex num_re(R"(^(\d+)[.):\s]+\s*(.*))");
    std::map<int, std::string> translations;

    std::size_t pos = 0;
    while (pos <= response_text.size()) {
        const std::size_t nl = response_text.find('\n', pos);
        std::string line = response_text.substr(
            pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? response_text.size() + 1 : nl + 1;

        line = trim(line);
        if (line.empty()) continue;
        // Strip a leading speaker tag if Claude included one anyway.
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

std::map<int, std::string> call_anthropic(
    anthropic::Client& client,
    const std::vector<std::pair<std::string, std::string>>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context) {
    bj::object body;
    body["model"] = MODEL;
    body["max_tokens"] = std::max<std::int64_t>(
        static_cast<std::int64_t>(batch_lines.size()) * 300, 16384);
    bj::object sys_block;
    sys_block["type"] = "text";
    sys_block["text"] = SYSTEM_PROMPT;
    sys_block["cache_control"] = bj::object{{"type", "ephemeral"}};
    body["system"] = bj::array{std::move(sys_block)};
    bj::object msg;
    msg["role"] = "user";
    msg["content"] = build_user_prompt(batch_lines, previous_context);
    body["messages"] = bj::array{std::move(msg)};

    if (MODEL == MODEL_SONNET) body["temperature"] = 0.3;
    // Reasoning-budget knob (per Anthropic docs): Opus 4.7 takes adaptive
    // thinking; Sonnet 4.6 / Opus 4.6 take output_config.effort.
    if (MODEL == MODEL_OPUS_47)
        body["thinking"] = bj::object{{"type", "adaptive"}};
    else if (MODEL == MODEL_SONNET || MODEL == MODEL_OPUS_46)
        body["output_config"] = bj::object{{"effort", EFFORT}};

    bj::value response = client.messages(bj::value(std::move(body)));

    // With adaptive/extended thinking the first content block is thinking;
    // translations live in the first block whose type == "text".
    std::string response_text;
    if (auto* content = response.get_object().if_contains("content"))
        for (const auto& block : content->get_array())
            if (auto* t = block.get_object().if_contains("type"))
                if (t->is_string() && t->get_string() == "text") {
                    if (auto* txt = block.get_object().if_contains("text"))
                        response_text = trim(std::string(txt->get_string()));
                    break;
                }

    log_info("\n--- CLAUDE'S RAW ANSWER ---");
    log_info(response_text);
    log_info("---------------------------\n");

    std::int64_t in_tok = 0, out_tok = 0, cache_read = 0, cache_create = 0;
    if (auto* usage = response.get_object().if_contains("usage")) {
        const auto& u = usage->get_object();
        auto geti = [&](const char* k) -> std::int64_t {
            if (auto* v = u.if_contains(k))
                if (v->is_int64()) return v->get_int64();
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

// --------------------------------------------------------------------------- //
// Per-file translation
// --------------------------------------------------------------------------- //

namespace {

struct Pending {
    std::size_t str_idx;
    std::string type;
    std::string speaker;
    std::string text;
};

// Returns (translated_count, stopped_early).
std::pair<long long, bool> translate_file(anthropic::Client& client, const std::string& fname,
                                          const bj::object& fdata, Cache& cache, int batch_size,
                                          const std::string& cache_file, int test_batches,
                                          bj::array* log_entries) {
    const std::vector<std::size_t> translatable = translatable_indices(fdata);
    if (translatable.empty()) return {0, false};

    const auto& strings = fdata.at("strings").get_array();
    std::vector<std::pair<std::string, std::string>> previous_context;  // (speaker, en)
    long long translated_count = 0;
    bool stopped = false;
    int api_batches_done = 0;

    for (std::size_t batch_start = 0; batch_start < translatable.size();
         batch_start += static_cast<std::size_t>(batch_size)) {
        const std::size_t batch_end = std::min(
            translatable.size(), batch_start + static_cast<std::size_t>(batch_size));

        std::vector<Pending> needs_api;
        for (std::size_t k = batch_start; k < batch_end; ++k) {
            const std::size_t idx = translatable[k];
            const auto& s = strings[idx].get_object();
            const std::string text(s.at("text").get_string());
            std::string type;
            if (auto* t = s.if_contains("type"))
                if (t->is_string()) type = std::string(t->get_string());
            const std::string speaker =
                type == "dialogue" ? extract_speaker(text) : std::string("NARRATION");
            if (const std::string* en = cache.get(text)) {
                previous_context.emplace_back(speaker, *en);
                ++translated_count;
            } else {
                needs_api.push_back(Pending{idx, type, speaker, text});
            }
        }

        if (needs_api.empty()) continue;

        ++api_batches_done;
        log_info("\n--- API batch #" + std::to_string(api_batches_done) + " (" +
                 std::to_string(needs_api.size()) + " new lines) ---");
        print_context_window(previous_context);

        std::vector<std::pair<std::string, std::string>> api_lines;
        api_lines.reserve(needs_api.size());
        for (const auto& p : needs_api) api_lines.emplace_back(p.speaker, p.text);

        auto apply = [&](const std::map<int, std::string>& translations, bool verbose) {
            for (std::size_t i = 0; i < needs_api.size(); ++i) {
                const auto& p = needs_api[i];
                auto it = translations.find(static_cast<int>(i) + 1);
                if (it != translations.end() && !it->second.empty()) {
                    const std::string tr = postprocess(p.text, it->second);
                    cache.set(p.text, tr);
                    previous_context.emplace_back(p.speaker, tr);
                    ++translated_count;
                    if (verbose) {
                        log_info("  JP: " + flatten_for_log(p.text, 60));
                        log_info("  EN: " + flatten_for_log(tr, 60));
                    }
                    if (log_entries) {
                        bj::object e;
                        e["script"] = fname;
                        e["line_idx"] = static_cast<std::int64_t>(p.str_idx);
                        e["type"] = p.type;
                        e["speaker"] = p.speaker;
                        e["japanese"] = p.text;
                        e["translated"] = tr;
                        log_entries->push_back(std::move(e));
                    }
                } else {
                    if (verbose)
                        log_info("    Missing translation for line " + std::to_string(i + 1) +
                                 ", keeping original");
                    previous_context.emplace_back(p.speaker, p.text);
                }
            }
        };

        try {
            auto translations = call_anthropic(client, api_lines, previous_context);
            if (translations.size() < needs_api.size())
                log_info("    WARNING: got " + std::to_string(translations.size()) + "/" +
                         std::to_string(needs_api.size()) + " translations");
            apply(translations, /*verbose=*/true);
        } catch (const anthropic::RateLimitError&) {
            log_info("    Rate limited, waiting 60s...");
            std::this_thread::sleep_for(std::chrono::seconds(60));
            try {
                apply(call_anthropic(client, api_lines, previous_context), /*verbose=*/false);
            } catch (const std::exception& e2) {
                log_info(std::string("    Retry failed: ") + e2.what() + ", keeping originals");
                for (const auto& p : needs_api) previous_context.emplace_back(p.speaker, p.text);
            }
        } catch (const std::exception& e) {
            log_info(std::string("    Batch error: ") + e.what());
            for (const auto& p : needs_api) previous_context.emplace_back(p.speaker, p.text);
        }

        if (previous_context.size() > CONTEXT_WINDOW * 2)
            previous_context.erase(previous_context.begin(),
                                   previous_context.end() - CONTEXT_WINDOW * 2);

        save_cache(cache, cache_file);

        if (test_batches && api_batches_done >= test_batches) {
            stopped = true;
            break;
        }
    }

    return {translated_count, stopped};
}

}  // namespace

// --------------------------------------------------------------------------- //
// Driver
// --------------------------------------------------------------------------- //

int translate_all(const TranslateOptions& opt) {
    log_info("Model: " + MODEL);
    if (is_effort_model(MODEL)) log_info("Effort: " + EFFORT);
    log_info("Batch size: " + std::to_string(opt.batch_size) + " lines per request");
    log_info("Context window: " + std::to_string(CONTEXT_WINDOW) + " previous lines");

    log_info("\nLoading extracted text...");
    bj::value all_data_v = json_parse_file(opt.input_file);
    bj::object& all_data = all_data_v.get_object();

    // The gate comes first, before an API key is read and before the client
    // is built: a run that cannot produce a correct answer must cost nothing
    // and leave nothing behind.
    log_info("\n--- Speaker gate (no tokens spent) ---");
    const SpeakerCheckReport gate = run_speaker_checks(all_data);
    if (!gate.passed) {
        log_info("\n[ERROR] speaker check " + std::to_string(gate.failed_check) +
                 " failed: " + gate.reason);
        log_info("        Nothing was translated and nothing was written.");
        return 2;
    }

    anthropic::load_api_key();
    anthropic::Client client;

    log_info("Loading translation cache...");
    Cache cache = load_cache(opt.cache_file);
    if (opt.retranslate) {
        log_info("  --retranslate: starting fresh cache");
        cache = Cache{};
    } else {
        log_info("  Cache has " + std::to_string(cache.size()) + " entries");
    }

    // Pre-populate name translations (unconditional set: the glossary wins).
    for (const auto& [jp, en] : name_translations_ordered()) cache.set(jp, en);

    // Story order first, then anything else in extraction order.
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
            if (!cache.contains(std::string(strings[i].get_object().at("text").get_string())))
                ++uncached;
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

            bj::array log_entries;
            auto [count, stopped] = translate_file(
                client, fname, all_data.at(fname).get_object(), cache, opt.batch_size,
                opt.cache_file, opt.test_batches, opt.test_batches ? &log_entries : nullptr);
            (void)count;
            done_lines += line_count;

            if (opt.test_batches && !log_entries.empty()) {
                fs::create_directories(fs::u8path(opt.test_dir));
                int run_num = 1;
                while (fs::exists(fs::u8path(opt.test_dir + "\\translation_run_" +
                                             std::to_string(run_num) + ".json")))
                    ++run_num;
                const std::string log_path =
                    opt.test_dir + "\\translation_run_" + std::to_string(run_num) + ".json";
                write_file_text(log_path, json_pretty(bj::value(log_entries), 2));
                log_info("  Test log: " + log_path + " (" +
                         std::to_string(log_entries.size()) + " entries)");
            }

            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            const double rate = elapsed > 0 ? static_cast<double>(done_lines) / elapsed : 0.0;
            char buf[160];
            std::snprintf(buf, sizeof(buf), "  Progress: %zu/%zu (%zu%%) | %.1f l/s", done_lines,
                          total_translatable,
                          total_translatable ? done_lines * 100 / total_translatable : 0, rate);
            log_info(buf);

            if (stopped) {
                log_info("\n  --test " + std::to_string(opt.test_batches) +
                         ": stopping after " + std::to_string(opt.test_batches) + " batch(es)");
                break;
            }
        }

        save_cache(cache, opt.cache_file);
        log_info("\nTranslation done! Cache: " + std::to_string(cache.size()) + " entries");
    }

    // Build final output.
    // postprocess() is re-run here, NOT just copied out of the cache: entries
    // translated before a name was added to the glossary have no speaker plate
    // in the cache, and this pass attaches it.  (The sibling BLACKCyc
    // pipelines copy the cache value verbatim -- do not "simplify" this to
    // match them.)
    log_info("\nBuilding translated text...");
    for (auto& kv : all_data) {
        auto* strings = kv.value().get_object().if_contains("strings");
        if (!strings || !strings->is_array()) continue;
        for (auto& sv : strings->get_array()) {
            auto& s = sv.get_object();
            const std::string text(s.at("text").get_string());
            const std::string* en = cache.get(text);
            s["translated"] = (en && *en != text) ? postprocess(text, *en) : text;
        }
    }

    write_file_text(opt.output_file, json_pretty(all_data_v, 1));
    char sz[64];
    std::snprintf(sz, sizeof(sz), "%.1f MB",
                  static_cast<double>(fs::file_size(fs::u8path(opt.output_file))) / 1024.0 /
                      1024.0);
    log_info("Saved to " + opt.output_file + " (" + sz + ")");
    return 0;
}

}  // namespace exc::translate
