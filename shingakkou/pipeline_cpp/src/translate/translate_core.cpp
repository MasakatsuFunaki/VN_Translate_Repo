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

#include "build_tsv/build_tsv.h"
#include "common/util.h"
#include "glossary.h"
#include "speaker_gate.h"

namespace shin::translate {

namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

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

bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

std::string json_str(const bj::object& o, const char* key) {
    if (auto* v = o.if_contains(key))
        if (v->is_string()) return std::string(v->get_string());
    return {};
}

}  // namespace

std::optional<std::string> validate_options(int batch_size, int test_n) {
    if (batch_size < 1)
        return "--batch must be at least 1 (got " + std::to_string(batch_size) + ")";
    if (test_n < 0)
        return "--test cannot be negative (got " + std::to_string(test_n) + ")";
    return std::nullopt;
}

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

bool is_control_code(const std::string& text) {
    if (contains(text, "@")) return true;
    // len(text) counts CODEPOINTS: a 20-character Japanese line is 60 bytes, so
    // a byte test would move the < 50 boundary and change which strings are
    // classified as sprite codes -- i.e. change the whole cache key set.
    if (contains(text, "_") && char_len(text) < 50 &&
        !contains(text, "\xE3\x80\x8C") &&   // 「
        !contains(text, "\xE3\x80\x8D") &&   // 」
        !contains(text, "\xE3\x80\x82") &&   // 。
        !contains(text, "\xE3\x80\x81") &&   // 、
        !contains(text, "\xE2\x80\xA6\xE2\x80\xA6") &&  // ……
        !contains(text, "\xE2\x80\x95") &&   // ― U+2015 HORIZONTAL BAR
        !contains(text, "\xE3\x80\x80"))     // U+3000 ideographic space
        return true;
    return false;
}

bool needs_translation(const std::string& text) {
    // trim removes U+3000 as well as ASCII whitespace, so a line of nothing
    // but ideographic spaces is rejected here rather than becoming a junk key.
    if (text.empty() || trim(text).empty()) return false;
    if (is_control_code(text)) return false;
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
        const std::string text = json_str(s, "text");
        const std::string type = json_str(s, "type");
        if (needs_translation(text) &&
            (type == "dialogue" || type == "narration" || type == "choice"))
            out.push_back(i);
    }
    return out;
}

std::string extract_speaker(const bj::object& s) {
    // Pattern A: read the `speaker` field 01_extract populated.  Parsing the
    // text instead (as the BLACKCyc ports must) would return NARRATION for
    // 100% of lines here -- exactly the regression speaker_gate exists to catch.
    const std::string speaker = json_str(s, "speaker");
    if (json_str(s, "type") == "dialogue" && !speaker.empty()) {
        const auto& map = name_translations();
        auto it = map.find(speaker);
        return it == map.end() ? speaker : it->second;
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

std::string postprocess(const std::string& original, const std::string& translated) {
    if (translated.empty() || translated == original) return translated;
    return sanitize_ascii(translated);
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
            // text[:100] is a CODEPOINT slice; cutting bytes would put a split
            // UTF-8 sequence straight into the request body.
            user_prompt += "[" + previous_context[i].first + "] " +
                           utf8_prefix(previous_context[i].second, 100) + "\n";
        user_prompt += "</previous_context>\n\n";
    }
    user_prompt += "<lines_to_translate>\n";
    for (std::size_t i = 0; i < batch_lines.size(); ++i)
        // Emitted RAW.  This pipeline's strings carry a LITERAL backslash-n as
        // their line break, so there is nothing to escape and escaping would
        // change the prompt bytes.
        user_prompt += std::to_string(i + 1) + ". [" + batch_lines[i].first + "] " +
                       batch_lines[i].second + "\n";
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
    // The '&' in the character class is not decoration: without it a leaked
    // "[Michael & Gabby]" tag stays glued to the front of the translation.
    static const boost::regex tag_re(R"(^\[(?:NARRATION|[A-Za-z\s()'?&]+)\]\s*)");
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
        // ^-anchored, and it runs BEFORE the number match: a tag that appears
        // after the number (e.g. "3. [NARRATION] text") is deliberately kept.
        line = boost::regex_replace(line, tag_re, "");
        boost::smatch m;
        if (boost::regex_match(line, m, num_re))
            // Escape sequences are left as-is: an un-escaped CR or LF in the
            // value would break the TSV row that carries it.
            translations[std::stoi(m[1].str())] = trim(m[2].str());
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
    // thinking; Sonnet 4.6 / Opus 4.6 take output_config.effort.  We build the
    // raw request body, so either field goes in at top level with no SDK
    // version to negotiate with.
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
    int batch_num = 0;
    // KNOWN DEFECT, deliberately preserved: api_batches_done is never
    // incremented (the batch loop bumps batch_num instead), so the --test N
    // stop condition below never fires and --test re-translates the whole file.
    // Repairing it changes what a smoke run produces and makes new runs
    // incomparable with every log already on disk, so it is left alone.
    int api_batches_done = 0;

    for (std::size_t batch_start = 0; batch_start < translatable.size();
         batch_start += static_cast<std::size_t>(batch_size)) {
        const std::size_t batch_end = std::min(
            translatable.size(), batch_start + static_cast<std::size_t>(batch_size));

        std::vector<Pending> needs_api;
        for (std::size_t k = batch_start; k < batch_end; ++k) {
            const std::size_t idx = translatable[k];
            const auto& s = strings[idx].get_object();
            const std::string text = json_str(s, "text");
            const std::string speaker = extract_speaker(s);
            if (const std::string* en = cache.get(text)) {
                previous_context.emplace_back(speaker, *en);
                ++translated_count;
            } else {
                needs_api.push_back(Pending{idx, json_str(s, "type"), speaker, text});
            }
        }

        if (needs_api.empty()) continue;

        ++batch_num;
        (void)batch_num;

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
                        log_info("  JP: " + utf8_prefix(p.text, 60));
                        log_info("  EN: " + utf8_prefix(tr, 60));
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
            // Logged BEFORE the apply loop, so the shortfall is visible above
            // the per-line output it explains.
            if (translations.size() < needs_api.size())
                log_info("    WARNING: got " + std::to_string(translations.size()) + "/" +
                         std::to_string(needs_api.size()) + " translations");
            apply(translations, /*verbose=*/true);
        } catch (const anthropic::RateLimitError&) {
            log_info("    Rate limited, waiting 60s...");
            std::this_thread::sleep_for(std::chrono::seconds(60));
            try {
                // The retry path logs nothing per line and writes no test-run
                // entries: a bare re-apply, so one rate-limit hiccup does not
                // duplicate a whole batch in the smoke log.
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

    // The gate comes first, before an API key is read and before a file is
    // removed: a run that cannot produce a correct answer must cost nothing
    // and leave nothing behind.
    log_info("\n--- Speaker gate (no tokens spent) ---");
    const SpeakerCheckReport gate = run_speaker_checks(all_data);
    if (!gate.passed) {
        log_info("\n[ERROR] speaker check " + std::to_string(gate.failed_check) +
                 " failed: " + gate.reason);
        log_info("        Nothing was translated and nothing was written.");
        return 2;
    }

    // A fresh run drops what earlier runs left, so the table this run produces
    // holds exactly what this run answered.  It happens after the gate for the
    // same reason the gate comes first.
    if (opt.fresh_run)
        for (const std::string& p : {opt.cache_file, opt.output_file})
            if (fs::exists(fs::u8path(p))) {
                fs::remove(fs::u8path(p));
                log_info("Fresh run: removed " + p);
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

    // Unconditional re-seed of the glossary.  Cache::set keeps an existing
    // key's position, which is what makes a cache-only re-run rewrite the file
    // unchanged instead of hoisting these 75 to the front.
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
        // No final save_cache on this path -- with nothing to translate the
        // cache file is left untouched rather than rewritten.
        log_info("  Everything is already cached!");
    } else {
        log_info("\n--- Translating ---");
        std::size_t done_lines = 0;
        auto start = std::chrono::steady_clock::now();

        for (const auto& [fname, line_count, uncached_count] : file_stats) {
            if (uncached_count == 0) {
                // Fully-cached files still count toward the progress total.
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

    // Materialise: EVERY file, ignoring --file.  "translated" is appended after
    // "text", which is the key order the reference JSON carries.
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

    // The table the game loads is derived from the document that was just
    // written, so it is built here rather than by a second command someone has
    // to remember: a translation the game never sees is not a translation.
    print_line("");
    log_info("Building the runtime translation table...");
    return build_tsv::run_build(opt.output_file, opt.tsv_file);
}

}  // namespace shin::translate
