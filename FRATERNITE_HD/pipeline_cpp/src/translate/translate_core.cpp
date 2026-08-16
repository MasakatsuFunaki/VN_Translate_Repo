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
#include <stdexcept>
#include <system_error>
#include <thread>

#include "build_tsv/build_tsv.h"
#include "common/util.h"
#include "glossary.h"
#include "speaker_gate.h"

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

// Renders a list of ints as "[1, 2, 4]" for the malformed-response warning.
std::string nums_repr(const std::vector<long long>& v) {
    std::string out = "[";
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out += ", ";
        out += std::to_string(v[i]);
    }
    return out + "]";
}

}  // namespace

// Declared in glossary.h but defined here: glossary.cpp is generated and must
// stay a verbatim table dump with no logic in it.
const std::vector<std::pair<std::string, std::string>>& names_by_len_desc() {
    static const std::vector<std::pair<std::string, std::string>> data = [] {
        std::vector<std::pair<std::string, std::string>> v = name_translations_ordered();
        // STABLE sort: equal-length JP keys must keep their declaration order,
        // or which of two same-length names wins a substitution flips.
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
    // A negative cap reads as "no limit" further down and would start an
    // unbounded run -- the opposite of what the flag is for.
    if (max_batches < 0)
        return "--max-batches cannot be negative (got " + std::to_string(max_batches) +
               "); use 0 for no limit";
    return std::nullopt;
}

bool has_real_japanese(const std::string& text) { return jp_char_count(text) > 0; }

bool needs_translation(const std::string& text) {
    if (text.empty() || trim(text).empty()) return false;
    if (char_len(text) <= 2 && !has_real_japanese(text)) return false;
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
    // Allowed = the complement of [　-〿「『（()0-9a-zA-Z\s].  「 and 『 sit
    // inside U+3000-303F, so they are already excluded by the range.
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
    // Every shorter prefix is followed by an Allowed char, which can never be
    // 「 or 『, so the greedy {1,12} can only succeed at exactly n.
    if (n == 0 || n > 12 || n >= cps.size()) return std::nullopt;
    if (cps[n] != kOpenBracket && cps[n] != kOpenDouble) return std::nullopt;
    return utf8_encode(std::vector<char32_t>(cps.begin(),
                                             cps.begin() + static_cast<std::ptrdiff_t>(n)));
}

std::string extract_speaker(const std::string& text) {
    const std::size_t crlf = text.find("\r\n");
    if (crlf != std::string::npos) {
        const std::string name = trim(text.substr(0, crlf));
        // Unlike the sibling games' pipelines, an unknown name falls back to
        // the RAW JP name rather than NARRATION: the lookup defaults to its
        // own key, so an off-glossary speaker still reads as a speaker.
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

    // Case A: Claude kept the brackets but left the JP name (男「Come on...」).
    // Longest-first so 園田 doesn't eat into 園田Ｈ; the loop BREAKS on the
    // first hit.
    const std::string open = utf8_encode_cp(kOpenBracket);
    for (const auto& [jp_name, en_name] : names_by_len_desc()) {
        if (en_name.empty()) continue;
        const std::string prefix = jp_name + open;
        if (t.rfind(prefix, 0) == 0) {
            t = en_name + open + t.substr(prefix.size());
            break;
        }
    }
    // Case B: Claude dropped the brackets entirely.
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
    // A cache is a JSON object of jp -> en.  get_object() rejects any other
    // shape without saying which file it read, and the caller is often about
    // to delete that file.
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

// Strip "^\[(?:NARRATION|[A-Za-z\s()'&?]+)\]\s*".  The NARRATION alternative is
// subsumed by the character class, so one class-based scan covers both.
// Note the '&' -- this game has speaker names like "R&B" that the sibling
// games' classes do not have to accept.
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

// Matches "^(\d+)[.):\s]+\s*(.*)" where \d is the UNICODE digit set: the model
// sometimes numbers a Japanese-heavy batch with fullwidth digits, so ASCII
// plus U+FF10-FF19 is the accepted range.
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
    // A number too large for long long can never equal any element of
    // [0..expected_count], so the -1 sentinel makes the sequence check REJECT
    // the whole batch instead of silently dropping the line and shifting the
    // rest by one.
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
// API call
// --------------------------------------------------------------------------- //

namespace {

// "a\r\nb" -> "a|b", truncated to `limit` CODEPOINTS (not bytes -- a byte cut
// would split a kanji and emit mojibake).
std::string flatten_for_log(const std::string& txt, std::size_t limit) {
    return cp_substr(replace_all(txt, "\r\n", "|"), 0, limit);
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
    const std::string n = std::to_string(batch_lines.size());
    user_prompt +=
        "Translate the " + n + " lines inside <lines_to_translate> to English. "
        "Use the <previous_context> (if present) for scene continuity but do NOT translate it. "
        "Output EXACTLY " + n + " numbered translations, one per line. "
        "Do NOT include the [SPEAKER] tag in your output -- it is metadata for context only. "
        "For dialogue lines that begin with a Japanese name + \xE3\x80\x8C...\xE3\x80\x8D in the "
        "source, KEEP the romanized name + \xE3\x80\x8C...\xE3\x80\x8D structure in your "
        "translation (e.g. \xE5\xA4\xA7\xE6\x99\xBA\xE3\x80\x8C\xE3\x82\x82\xE3\x81\x97\xE3\x82"
        "\x88\xE3\x81\x97\xE3\x80\x8D -> Daichi\xE3\x80\x8CHey there\xE3\x80\x8D).";
    return user_prompt;
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

    // With adaptive thinking the first content block is thinking; the
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

    return parse_translations(response_text, batch_lines.size());
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
std::pair<long long, bool> translate_file(anthropic::LazyClient& client, const std::string& fname,
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
            const std::string text = json_str(s, "text");
            // YU-RIS inlines `Name「...」` even inside lines tagged "narrative",
            // so always probe rather than gating on type.
            const std::string speaker = extract_speaker(text);
            if (const std::string* en = cache.get(text)) {
                previous_context.emplace_back(speaker, *en);
                ++translated_count;
            } else {
                needs_api.push_back(Pending{idx, json_str(s, "type"), speaker, text});
            }
        }

        if (needs_api.empty()) continue;

        // The first batch the cache cannot cover is where a key becomes
        // required.  Outside the try below on purpose: that handler catches
        // every exception, so a missing key would be logged once per batch and
        // the run would still finish, leaving a table full of Japanese.
        anthropic::Client& api = client.get();

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
                    // Reject outputs that still contain Japanese -- don't
                    // pollute the cache; the JAPANESE goes into the context and
                    // the line is neither cached nor counted.
                    if (en_has_japanese_content(tr)) {
                        log_info("    SKIP (JP in EN, not cached): " + cp_substr(tr, 0, 80));
                        previous_context.emplace_back(p.speaker, p.text);
                        continue;
                    }
                    cache.set(p.text, tr);
                    previous_context.emplace_back(p.speaker, tr);
                    ++translated_count;
                    if (verbose) {
                        log_info("  JP: " + flatten_for_log(p.text, 60));
                        log_info("  EN: " + flatten_for_log(tr, 60));
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
            auto translations = call_anthropic(api, api_lines, previous_context);
            if (translations.size() < needs_api.size())
                log_info("    WARNING: got " + std::to_string(translations.size()) + "/" +
                         std::to_string(needs_api.size()) + " translations");
            apply(translations, /*verbose=*/true);
        } catch (const anthropic::RateLimitError&) {
            log_info("    Rate limited, waiting 60s...");
            std::this_thread::sleep_for(std::chrono::seconds(60));
            try {
                apply(call_anthropic(api, api_lines, previous_context), /*verbose=*/false);
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
    // A run that discards the cache is certain to call the API, so the key is
    // required before anything is deleted rather than at the first batch.
    if (opt.fresh_run && anthropic::load_api_key().empty()) {
        log_info("[ERROR] ANTHROPIC_API_KEY not set -- refusing to discard the "
                 "cache for a run that cannot translate.");
        return 1;
    }

    if (opt.fresh_run)
        for (const std::string& p : {opt.cache_file, opt.output_file}) {
            // Reported, not thrown, at both steps: the throwing overloads name
            // no file, and this run is about to translate from scratch.  A
            // delete fails when another process holds the file open without
            // FILE_SHARE_DELETE, which scanners, indexers and sync clients
            // routinely do.
            const fs::path path = fs::u8path(p);
            std::error_code ec;
            const bool present = fs::exists(path, ec);
            if (!ec && !present) continue;  // never written: nothing to remove
            const bool removed = !ec && fs::remove(path, ec);
            if (ec) {
                log_info("[ERROR] cannot delete " + p + ": " + ec.message() +
                         " -- close whatever has it open, then retry.");
                return 1;
            }
            // A file that vanished between the two calls was not removed here.
            log_info(removed ? "Fresh run: removed " + p
                             : "Fresh run: " + p + " was already gone");
        }

    anthropic::LazyClient client;

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

    // Pre-populate name translations (unconditional set: the glossary wins,
    // and re-setting an existing key does NOT move it in the ordering).
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
        // save_cache stays inside the else branch on purpose: with everything
        // already cached the file must NOT be rewritten, even though the
        // in-memory cache was mutated by the glossary pre-population.
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
                opt.cache_file, opt.test_batches, opt.test_mode ? &log_entries : nullptr);
            (void)count;
            done_lines += line_count;

            if (opt.test_mode && !log_entries.empty()) {
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

    // The runtime table is built here, in the same command that paid for the
    // translations, so there is no step to remember between translating and
    // playing.  Both paths above reach this line, including the one where
    // everything was already cached.
    log_info("\n--- Building the runtime translation table ---");
    return build_tsv::run_build(opt.output_file, opt.tsv_file);
}

}  // namespace frat::translate
