// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "charts.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <set>
#include <thread>
#include <unordered_map>

#include <boost/json.hpp>
#include <boost/regex.hpp>

#include "translate/anthropic_client.h"
#include "translate/translate_core.h"

namespace exc::charts {

namespace bj = boost::json;
namespace fs = std::filesystem;

namespace {

constexpr const char* MODEL = "claude-sonnet-4-6";
constexpr int BATCH_SIZE = 30;

// Credits chart: staff names must not be translated.
const std::set<std::string>& skip_files() {
    static const std::set<std::string> s = {"chart9.fxf"};
    return s;
}

const char* SYSTEM_PROMPT =
    R"(You are a professional Japanese-to-English translator for the visual novel "EXTRAVAGANZA ~Mushi Mederu Shoujo~".

This is a dark supernatural horror VN about Bug Users (蟲使い) who wield parasitic insects called 蟲 (Bugs).
Key characters: Rengo (煉悟) - a rogue Bug User, Yumemi (夢美) - a captive girl, Ageha (アゲハ) - a mysterious Bug User,
Sayuri (サユリ) - a pursuing Bug User, Yuria (ユーリア) - an associate, Nishi (西) - a researcher,
Ren (レン) - an enemy Bug User, Kyouko (杏子), Miyaka (美弥香), Yui (唯), Ayaka (綾佳), Haruka (遥).

IMPORTANT: Translate ALL content faithfully. This game contains horror and adult themes — do NOT censor or skip anything.

<translation_rules>
1. Translate the Japanese text to natural English. These are episode titles and brief descriptions for a flowchart menu.
2. Keep translations concise — these appear in small UI boxes.
3. Preserve all formatting markers EXACTLY: #色 color tags (#黄, #赤, #桃, etc.), 【】 brackets, and #心 markers.
4. Use only ASCII characters in the translated text (no smart quotes, em dashes, or accented letters).
5. Output EXACTLY one translation per numbered input line.
</translation_rules>)";

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

std::vector<std::string> split(const std::string& s, const std::string& sep) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t hit = s.find(sep, pos);
        if (hit == std::string::npos) {
            out.push_back(s.substr(pos));
            return out;
        }
        out.push_back(s.substr(pos, hit - pos));
        pos = hit + sep.size();
    }
}

std::string join(const std::vector<std::string>& v, const std::string& sep) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out += sep;
        out += v[i];
    }
    return out;
}

std::size_t char_len(const std::string& s) { return utf8_decode(s).size(); }

constexpr const char* OPEN_BRACKET = "\xE3\x80\x90";   // 【
constexpr const char* CLOSE_BRACKET = "\xE3\x80\x91";  // 】
constexpr char32_t IDEOGRAPHIC_SPACE = 0x3000;

}  // namespace

// ---- FXF envelope ----------------------------------------------------------

Bytes encrypt_fxf(const Bytes& data) {
    Bytes out(data.size());
    for (std::size_t i = 0; i < data.size(); ++i)
        out[i] = static_cast<std::uint8_t>(data[i] ^ 0xFF);
    return out;
}

Bytes decrypt_fxf(const Bytes& data) { return encrypt_fxf(data); }

// ---- text helpers ----------------------------------------------------------

std::string to_fullwidth(const std::string& text) {
    const std::vector<char32_t> cps = utf8_decode(text);
    std::string out;
    for (std::size_t i = 0; i < cps.size(); ++i) {
        const char32_t ch = cps[i];
        // '#' followed by a non-ASCII char is a colour tag (#赤, #黄, #心, ...)
        // and must stay single-byte.
        if (ch == U'#' && i + 1 < cps.size() && cps[i + 1] > 0x7F) {
            out += utf8_encode_cp(ch);
        } else if (ch >= 0x21 && ch <= 0x7E) {
            out += utf8_encode_cp(ch - 0x21 + 0xFF01);
        } else if (ch == U' ') {
            out += utf8_encode_cp(IDEOGRAPHIC_SPACE);
        } else {
            out += utf8_encode_cp(ch);
        }
    }
    return out;
}

bool has_japanese(const std::string& text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const char32_t cp = utf8_next(text, i);
        if ((cp >= 0x3040 && cp <= 0x309F) || (cp >= 0x30A0 && cp <= 0x30FF) ||
            (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF))
            return true;
    }
    return false;
}

bool parse_chart_line(const std::string& raw, std::string* prefix,
                      std::vector<std::string>* segments) {
    segments->clear();
    // Trim the line terminator ONLY: a trailing full-width space is part of
    // the node's layout and must survive into the rewritten line.
    std::string line = raw;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    if (line.empty() || line[0] != '@') {
        prefix->clear();
        return false;
    }

    const std::size_t first_hash = line.find('#');
    if (first_hash == std::string::npos) {
        *prefix = line;
        return true;  // node line, but nothing translatable
    }

    *prefix = trim_right(line.substr(0, first_hash));
    const std::string text_part = line.substr(first_hash);

    // The alt segment starts at ' !#'.
    const std::size_t alt_start = text_part.find(" !#");
    if (alt_start != std::string::npos) {
        segments->push_back(text_part.substr(0, alt_start));
        segments->push_back(text_part.substr(alt_start + 1));  // keep the '!'
    } else {
        segments->push_back(text_part);
    }
    return true;
}

Segment extract_translatable_text(const std::string& segment_in) {
    Segment out;
    std::string segment = segment_in;
    out.is_alt = !segment.empty() && segment[0] == '!';
    if (out.is_alt) segment = segment.substr(1);

    // Shape: '#<colour>【<title>】<space*><description>', where <colour> is
    // exactly ONE CODEPOINT and the whitespace run after '】' is Unicode
    // whitespace -- it is usually the U+3000 these files are full of.  Both
    // rules are why this is hand-rolled instead of a byte-oriented regex.
    const std::vector<char32_t> cps = utf8_decode(segment);
    bool matched = false;
    if (cps.size() >= 3 && cps[0] == U'#') {
        const std::vector<char32_t> open = utf8_decode(OPEN_BRACKET);
        if (cps[2] == open[0]) {
            // Title runs up to the FIRST '】', never a later one.
            const std::vector<char32_t> close = utf8_decode(CLOSE_BRACKET);
            std::size_t close_i = std::string::npos;
            for (std::size_t i = 3; i < cps.size(); ++i)
                if (cps[i] == close[0]) { close_i = i; break; }
            // A title needs at least one character before '】'.
            if (close_i != std::string::npos && close_i > 3) {
                std::size_t mid_end = close_i + 1;
                while (mid_end < cps.size() && is_unicode_space(cps[mid_end])) ++mid_end;
                out.pre_title = utf8_encode({cps.begin(), cps.begin() + 3});
                out.title = utf8_encode({cps.begin() + 3, cps.begin() + static_cast<std::ptrdiff_t>(close_i)});
                out.mid = utf8_encode({cps.begin() + static_cast<std::ptrdiff_t>(close_i),
                                       cps.begin() + static_cast<std::ptrdiff_t>(mid_end)});
                out.desc = utf8_encode({cps.begin() + static_cast<std::ptrdiff_t>(mid_end), cps.end()});
                matched = true;
            }
        }
    }
    if (!matched) {
        // Fallback: the whole thing is the title.
        out.pre_title.clear();
        out.title = segment;
        out.mid.clear();
        out.desc.clear();
    }
    return out;
}

std::string strip_alt_marks(const std::string& s) {
    // Each '!' and the whitespace run before it collapse to a single space;
    // the whitespace test is Unicode-wide, so U+3000 counts.
    const std::vector<char32_t> cps = utf8_decode(s);
    std::string out;
    std::size_t i = 0;
    while (i < cps.size()) {
        std::size_t j = i;
        while (j < cps.size() && is_unicode_space(cps[j])) ++j;
        if (j < cps.size() && cps[j] == U'!') {
            out += ' ';
            i = j + 1;
            continue;
        }
        out += utf8_encode_cp(cps[i]);
        ++i;
    }
    return trim(out);
}

std::string wordwrap_fullwidth(const std::string& text, int max_chars, int max_lines) {
    const std::vector<char32_t> cps = utf8_decode(text);
    std::string out;
    int col = 0, line_count = 1;
    for (char32_t ch : cps) {
        out += utf8_encode_cp(ch);
        ++col;
        if (col >= max_chars && ch == IDEOGRAPHIC_SPACE) {
            if (line_count >= max_lines) break;
            out += "\r\n";
            col = 0;
            ++line_count;
        }
    }
    return out;
}

// ---- API -------------------------------------------------------------------

namespace {

std::map<int, std::string> call_anthropic(anthropic::Client& client,
                                          const std::vector<std::string>& texts) {
    std::string user_prompt = "<texts_to_translate>\n";
    for (std::size_t i = 0; i < texts.size(); ++i)
        user_prompt += std::to_string(i + 1) + ". " + texts[i] + "\n";
    user_prompt += "</texts_to_translate>\n\n";
    user_prompt += "Translate these " + std::to_string(texts.size()) +
                   " flowchart texts to English. "
                   "Preserve #色 color tags, 【】 brackets, and #心 markers exactly. "
                   "Output EXACTLY " + std::to_string(texts.size()) + " numbered translations.";

    bj::object sys_block;
    sys_block["type"] = "text";
    sys_block["text"] = SYSTEM_PROMPT;
    sys_block["cache_control"] = bj::object{{"type", "ephemeral"}};
    bj::object msg;
    msg["role"] = "user";
    msg["content"] = user_prompt;

    bj::object body;
    body["model"] = MODEL;
    body["max_tokens"] = 4096;
    body["system"] = bj::array{std::move(sys_block)};
    body["messages"] = bj::array{std::move(msg)};
    body["temperature"] = 0.3;

    bj::value response = client.messages(bj::value(std::move(body)));

    std::string response_text;
    if (auto* content = response.get_object().if_contains("content"))
        if (!content->get_array().empty())
            if (auto* t = content->get_array()[0].get_object().if_contains("text"))
                response_text = trim(std::string(t->get_string()));

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

    static const boost::regex num_re(R"(^(\d+)[.\):\s]+\s*(.*))");
    std::map<int, std::string> translations;
    for (const auto& raw_line : split(response_text, "\n")) {
        const std::string line = trim(raw_line);
        if (line.empty()) continue;
        boost::smatch m;
        if (boost::regex_match(line, m, num_re))
            translations[std::stoi(m[1].str())] = trim(m[2].str());
    }
    return translations;
}

std::vector<std::string> chart_files(const std::string& chart_dir) {
    std::vector<std::string> out;
    for (const auto& e : fs::directory_iterator(fs::u8path(chart_dir))) {
        const std::string name = e.path().filename().u8string();
        if (name.size() < 4 || name.compare(name.size() - 4, 4, ".fxf") != 0) continue;
        if (skip_files().count(name)) continue;
        out.push_back(name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string decode_chart(const std::string& path) {
    const Bytes dec = decrypt_fxf(read_file(path));
    return cp932_to_utf8_replace(dec.data(), dec.size());
}

}  // namespace

Cache load_cache(const std::string& path) {
    Cache c;
    if (!fs::exists(fs::u8path(path))) return c;
    bj::value root = json_parse_file(path);
    // A cache is a JSON object of jp -> en.  get_object() rejects any other
    // shape without saying which file it read.
    if (!root.is_object())
        throw std::runtime_error("cannot parse " + path +
                                 ": a translation cache must be a JSON object");
    for (const auto& kv : root.get_object())
        if (kv.value().is_string())
            c.set(std::string(kv.key()), std::string(kv.value().get_string()));
    return c;
}

void save_cache(const Cache& c, const std::string& path) {
    bj::object o;
    for (const auto& [k, v] : c.items()) o[k] = v;
    write_file_text(path, json_pretty(bj::value(std::move(o)), 1));
}

std::size_t purge_failed_entries(Cache& cache) {
    std::size_t removed = 0;
    Cache kept;
    for (const auto& [jp, en] : cache.items()) {
        if (translate::is_failed_entry(jp, en)) ++removed;
        else kept.set(jp, en);
    }
    if (removed) cache = std::move(kept);
    return removed;
}

int run_translate_charts(const ChartOptions& opt) {
    log_info(std::string("Model: ") + MODEL);
    log_info("Chart directory: " + opt.chart_dir);

    Cache cache;
    if (opt.retranslate) {
        log_info("Cache ignored (--retranslate)");
    } else {
        cache = load_cache(opt.cache_file);
        log_info("Cache has " + std::to_string(cache.size()) + " entries");
        const std::size_t purged = purge_failed_entries(cache);
        log_info("Failed entries purged: " + std::to_string(purged) +
                 (purged ? "  (those lines re-queue)" : ""));
    }

    fs::create_directories(fs::u8path(opt.backup_dir));

    const std::vector<std::string> files = chart_files(opt.chart_dir);

    // ---- pass 1: collect every text needing translation ----
    // De-duplicated but kept in first-seen order, so batch composition (and
    // therefore the run) is reproducible.
    std::vector<std::string> to_translate;
    std::set<std::string> seen;
    auto want = [&](const std::string& t) {
        if (t.empty() || !has_japanese(t) || cache.contains(t)) return;
        if (seen.insert(t).second) to_translate.push_back(t);
    };

    for (const auto& fname : files) {
        const std::string text = decode_chart(opt.chart_dir + "\\" + fname);
        for (const auto& line : split(text, "\n")) {
            std::string prefix;
            std::vector<std::string> segments;
            if (!parse_chart_line(line, &prefix, &segments)) continue;
            for (const auto& seg : segments) {
                const Segment s = extract_translatable_text(seg);
                want(s.title);
                if (!s.desc.empty() && has_japanese(s.desc))
                    want(s.is_alt ? strip_alt_marks(s.desc) : s.desc);
            }
        }
    }

    log_info("\nTexts to translate: " + std::to_string(to_translate.size()) +
             " (cached: skipped)");

    if (opt.test_mode) {
        log_info("\n--- DRY RUN: showing first 20 texts ---");
        std::vector<std::string> sorted_texts = to_translate;
        std::sort(sorted_texts.begin(), sorted_texts.end());
        for (std::size_t i = 0; i < sorted_texts.size() && i < 20; ++i)
            log_info("  " + std::to_string(i + 1) + ". " + sorted_texts[i]);
        return 0;
    }

    // ---- translate in batches ----
    if (!to_translate.empty()) {
        anthropic::Client client;
        for (std::size_t start = 0; start < to_translate.size(); start += BATCH_SIZE) {
            const std::size_t end = std::min(to_translate.size(), start + BATCH_SIZE);
            const std::vector<std::string> batch(to_translate.begin() + static_cast<std::ptrdiff_t>(start),
                                                 to_translate.begin() + static_cast<std::ptrdiff_t>(end));
            log_info("\n  Batch " + std::to_string(start / BATCH_SIZE + 1) + ": " +
                     std::to_string(batch.size()) + " texts");

            auto apply = [&](const std::map<int, std::string>& tr, bool warn) {
                for (std::size_t i = 0; i < batch.size(); ++i) {
                    auto it = tr.find(static_cast<int>(i) + 1);
                    if (it != tr.end() && !it->second.empty()) cache.set(batch[i], it->second);
                    else if (warn)
                        log_info("    WARNING: missing translation for item " +
                                 std::to_string(i + 1));
                }
                save_cache(cache, opt.cache_file);
            };

            try {
                apply(call_anthropic(client, batch), /*warn=*/true);
            } catch (const anthropic::RateLimitError&) {
                log_info("    Rate limited, waiting 60s...");
                std::this_thread::sleep_for(std::chrono::seconds(60));
                try {
                    apply(call_anthropic(client, batch), /*warn=*/false);
                } catch (const std::exception& e) {
                    log_info(std::string("    Retry failed: ") + e.what());
                }
            } catch (const std::exception& e) {
                log_info(std::string("    Batch error: ") + e.what());
            }
        }
        save_cache(cache, opt.cache_file);
        log_info("\nTranslation done. Cache: " + std::to_string(cache.size()) + " entries");
    }

    // ---- pass 2: repack, longest key first so a short key cannot match
    // inside a longer one (e.g. 墓 inside 少女は、墓を作って...) ----
    std::vector<std::pair<std::string, std::string>> sorted_cache = cache.items();
    std::stable_sort(sorted_cache.begin(), sorted_cache.end(),
                     [](const auto& a, const auto& b) {
                         return char_len(a.first) > char_len(b.first);
                     });

    log_info("\n--- Repacking chart files ---");
    for (const auto& fname : files) {
        const std::string filepath = opt.chart_dir + "\\" + fname;
        const std::string backup_path = opt.backup_dir + "\\" + fname;
        if (!fs::exists(fs::u8path(backup_path)))
            fs::copy_file(fs::u8path(filepath), fs::u8path(backup_path));

        // Always read the pristine backup, so repacking is idempotent.
        const std::string text = decode_chart(backup_path);

        long long changes = 0;
        std::vector<std::string> new_lines;
        for (const auto& line : split(text, "\r\n")) {
            if (line.empty() || line[0] != '@') {
                new_lines.push_back(line);
                continue;
            }
            // The alt section is rebuilt separately, not string-replaced.
            const std::size_t alt_idx = line.find(" !#");
            std::string main_part =
                alt_idx == std::string::npos ? line : line.substr(0, alt_idx);
            std::string alt_part =
                alt_idx == std::string::npos ? std::string() : line.substr(alt_idx);

            for (const auto& [jp_text, en_text] : sorted_cache) {
                if (en_text == jp_text) continue;
                if (main_part.find(jp_text) == std::string::npos) continue;
                std::string fw_en = to_fullwidth(en_text);
                if (char_len(jp_text) > 10) fw_en = wordwrap_fullwidth(fw_en, 42);
                main_part = replace_all(main_part, jp_text, fw_en);
                ++changes;
            }

            if (!alt_part.empty()) {
                // Same '#<colour>【title】 description' shape as a main
                // segment, once the leading space and '!' are off.
                const Segment s = extract_translatable_text(trim_left(alt_part));
                if (!s.pre_title.empty()) {
                    const std::string alt_pre = " " + s.pre_title;
                    std::string new_alt_title = s.title;
                    if (has_japanese(s.title) && cache.contains(s.title)) {
                        new_alt_title = to_fullwidth(*cache.get(s.title));
                        ++changes;
                    }
                    const std::string clean_alt_desc = strip_alt_marks(s.desc);
                    if (has_japanese(clean_alt_desc) && cache.contains(clean_alt_desc)) {
                        std::string fw_alt_desc = to_fullwidth(*cache.get(clean_alt_desc));
                        if (char_len(clean_alt_desc) > 10)
                            fw_alt_desc = wordwrap_fullwidth(fw_alt_desc, 42);
                        // The engine requires a '!' on every alt-section line.
                        fw_alt_desc = "!" + replace_all(fw_alt_desc, "\r\n", "\r\n!");
                        alt_part = alt_pre + new_alt_title + s.mid + fw_alt_desc;
                        ++changes;
                    }
                }
            }

            new_lines.push_back(main_part + alt_part);
        }

        write_file(filepath, encrypt_fxf(utf8_to_cp932_replace(join(new_lines, "\r\n"))));
        log_info("  " + fname + ": " + std::to_string(changes) + " replacements");
    }

    log_info("\nRepack complete. Backups in: " + opt.backup_dir);
    return 0;
}

}  // namespace exc::charts
