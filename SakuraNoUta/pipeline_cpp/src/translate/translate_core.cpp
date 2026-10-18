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
#include <system_error>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <thread>

#include <boost/regex.hpp>

#include "build_tsv/build_tsv.h"
#include "common/util.h"
#include "glossary.h"
#include "speaker_gate.h"

namespace snu::translate {

namespace bj = boost::json;
namespace fs = std::filesystem;

bool needs_translation(const std::string& text) {
    if (text.empty()) return false;
    return is_jp(text);
}

std::string strip(const std::string& s) {
    std::size_t b = 0, e = s.size();
    auto is_ws = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
    };
    while (b < e && is_ws(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && is_ws(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

namespace {

bool is_ascii_blank(char c) { return c == ' ' || c == '\t'; }

}  // namespace

std::string escape_line_breaks(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r') {
            out += "\\n";
            if (i + 1 < s.size() && s[i + 1] == '\n') ++i;  // CRLF is one break
        } else if (s[i] == '\n') {
            out += "\\n";
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string unescape_line_breaks(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n') {
            // Strip ASCII blanks around the escape; leave ideographic space (the real indent).
            while (!out.empty() && is_ascii_blank(out.back())) out.pop_back();
            out += '\n';
            ++i;
            while (i + 1 < s.size() && is_ascii_blank(s[i + 1])) ++i;
            continue;
        }
        out += s[i];
    }
    return out;
}

std::string strip_speaker_tags(const std::string& text) {
    // Anchored so it only strips a tag the model echoed at the front.
    static const boost::regex re(
        R"(^\[(?:NARRATION|[A-Za-z0-9\s()'\"?&$.,!-]+)\]\s*)");
    return strip(boost::regex_replace(text, re, ""));
}

std::int64_t compute_max_tokens(int batch_size) {
    std::int64_t want = static_cast<std::int64_t>(std::max(batch_size, 0)) *
                        OUTPUT_TOKENS_PER_LINE;
    return std::clamp(want, MIN_OUTPUT_TOKENS, MAX_OUTPUT_TOKENS);
}

std::optional<std::string> validate_options(int batch_size, int test_n, int max_batches) {
    if (batch_size < 1)
        return "--batch must be at least 1 (got " + std::to_string(batch_size) + ")";
    if (test_n < 0)
        return "--test cannot be negative (got " + std::to_string(test_n) + ")";
    // Negative would read as "no limit" and start an unbounded run.
    if (max_batches < 0)
        return "--max-batches cannot be negative (got " + std::to_string(max_batches) +
               "); use 0 for no limit";
    return std::nullopt;
}

std::string extract_speaker(const bj::object& entry) {
    if (auto* t = entry.if_contains("type"))
        if (t->is_string() && t->get_string() == "name")
            return "NAME";
    std::string raw;
    if (auto* s = entry.if_contains("speaker"))
        if (s->is_string()) raw = strip(std::string(s->get_string()));
    if (!raw.empty()) {
        const auto& map = name_translations();
        auto it = map.find(raw);
        return it == map.end() ? raw : it->second;
    }
    return "NARRATION";
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
    for (const auto& [jp, en] : cache.items())
        o[jp] = en;
    write_file_atomic(cache_file, json_pretty(bj::value(std::move(o)), 2));
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

namespace {

// Answers whose source line break was silently dropped.  Counted, not gated.
std::size_t& lost_line_break_events() {
    static std::size_t count = 0;
    return count;
}

// True when a break sits inside the line, not just trailing at the end.
bool has_internal_line_break(const std::string& s) {
    std::size_t end = s.size();
    while (end > 0 && (s[end - 1] == '\n' || s[end - 1] == '\r')) --end;
    return s.find_first_of("\r\n") < end;
}

}  // namespace

void apply_line_result(Cache& cache,
                       std::vector<std::pair<std::string, std::string>>& previous_context,
                       const std::string& speaker, const std::string& jp,
                       const std::string& en) {
    if (en.empty()) {
        previous_context.emplace_back(speaker, jp);
        return;
    }
    if (has_internal_line_break(jp) && !has_internal_line_break(en))
        log_info("    NOTE: the answer dropped the line break its source carries (" +
                 std::to_string(++lost_line_break_events()) +
                 " so far this run) -- caching it as one line: " + en.substr(0, 80));
    cache.set(jp, en);
    previous_context.emplace_back(speaker, en);
}

// --------------------------------------------------------------------------- //
// API call
// --------------------------------------------------------------------------- //

namespace {

std::string& raw_answer_log_slot() {
    static std::string path;
    return path;
}

// Batches whose last entry had trailing text appended after it.
std::size_t& trailing_text_events() {
    static std::size_t count = 0;
    return count;
}

constexpr std::size_t TRAILING_TEXT_PREVIEW = 300;

// Append the full answer under a greppable header (batch info + cost).
void append_raw_answer(const std::string& summary, const std::string& answer) {
    const std::string& path = raw_answer_log_slot();
    if (path.empty()) return;
    std::ofstream f(fs::u8path(path), std::ios::binary | std::ios::app);
    if (!f) return;
    f << "=== " << summary << " ===\n" << answer << "\n\n";
}

void print_context_window(
    const std::vector<std::pair<std::string, std::string>>& previous_context) {
    std::size_t n = std::min<std::size_t>(previous_context.size(), CONTEXT_WINDOW);
    log_info("\n=== ROLLING CONTEXT (last " + std::to_string(n) + " of " +
             std::to_string(previous_context.size()) + ") ===");
    if (n == 0) {
        log_info("  (empty -- start of script)");
    } else {
        for (std::size_t i = previous_context.size() - n; i < previous_context.size(); ++i) {
            const auto& [speaker, txt] = previous_context[i];
            std::string short_txt;
            for (std::size_t k = 0; k < txt.size(); ++k) {
                if (txt.compare(k, 2, "\r\n") == 0) { short_txt += '|'; ++k; }
                else if (txt[k] == '\n') short_txt += '|';
                else short_txt += txt[k];
            }
            if (short_txt.size() > 120) short_txt.resize(120);
            log_info("  [" + speaker + "] " + short_txt);
        }
    }
    log_info("=== END CONTEXT ===\n");
}

}  // namespace

void set_raw_answer_log(const std::string& path) { raw_answer_log_slot() = path; }

const std::string& raw_answer_log() { return raw_answer_log_slot(); }

std::string raw_answer_log_path(const std::string& sibling_file) {
    fs::path p = fs::u8path(sibling_file);
    return (p.parent_path() / fs::u8path("anthropic_raw_answers.log")).u8string();
}

std::string build_user_prompt(
    const std::vector<std::pair<std::string, std::string>>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context) {
    std::string user_prompt;
    if (!previous_context.empty()) {
        user_prompt += "<previous_context>\n";
        std::size_t start = previous_context.size() > CONTEXT_WINDOW
                                ? previous_context.size() - CONTEXT_WINDOW
                                : 0;
        for (std::size_t i = start; i < previous_context.size(); ++i)
            user_prompt += "[" + previous_context[i].first + "] " +
                           escape_line_breaks(previous_context[i].second) + "\n";
        user_prompt += "</previous_context>\n\n";
    }
    user_prompt += "<lines_to_translate>\n";
    for (std::size_t i = 0; i < batch_lines.size(); ++i)
        user_prompt += std::to_string(i + 1) + ". [" + batch_lines[i].first + "] " +
                       escape_line_breaks(batch_lines[i].second) + "\n";
    user_prompt += "</lines_to_translate>\n\n";
    user_prompt +=
        "Translate the " + std::to_string(batch_lines.size()) +
        " numbered lines inside <lines_to_translate>. "
        "Use <previous_context> (if present) for scene continuity but do NOT translate it. "
        "Output EXACTLY " + std::to_string(batch_lines.size()) +
        " numbered translations, one per line, "
        "following the formatting rules in the system prompt.";
    return user_prompt;
}

namespace {

// U+3000 IDEOGRAPHIC SPACE: paragraph indent the answer may carry or use as separator.
constexpr char IDEOGRAPHIC_SPACE[] = "\xE3\x80\x80";
constexpr std::size_t IDEOGRAPHIC_SPACE_LEN = 3;

// More digits than this is a price or year, not a line number.
constexpr int MAX_LINE_NUMBER_DIGITS = 9;

bool is_blank(char c) { return c == ' ' || c == '\t'; }

bool ideographic_space_at(const std::string& s, std::size_t i) {
    return s.compare(i, IDEOGRAPHIC_SPACE_LEN, IDEOGRAPHIC_SPACE) == 0;
}

// ASCII or fullwidth digit at `i`.  Returns byte length, 0 if none.
std::size_t digit_at(const std::string& s, std::size_t i, int* value) {
    if (i >= s.size()) return 0;
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c >= '0' && c <= '9') {
        if (value) *value = c - '0';
        return 1;
    }
    if (i + 2 < s.size() && c == 0xEF && static_cast<unsigned char>(s[i + 1]) == 0xBC) {
        unsigned char d = static_cast<unsigned char>(s[i + 2]);
        if (d >= 0x90 && d <= 0x99) {
            if (value) *value = d - 0x90;
            return 3;
        }
    }
    return 0;
}

// What the model can write between a line number and the translation.
enum class Mark {
    None,    // no mark at all, so the line is ordinary prose
    Period,  // '.' / U+FF0E -- the mark build_user_prompt itself writes
    Other,   // ')' ':' 。 、 and the fullwidth ')' ':' -- marks the model
             // writes instead of the one it was given
    Hyphen,  // '-' -- numbering-shaped, and never an opener
};

// Punctuation mark at `i` (ASCII and fullwidth/CJK).  Returns byte length, 0 if none.
std::size_t mark_at(const std::string& s, std::size_t i, Mark* kind) {
    *kind = Mark::None;
    if (i >= s.size()) return 0;
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == '.') { *kind = Mark::Period; return 1; }
    if (c == ')' || c == ':') { *kind = Mark::Other; return 1; }
    if (c == '-') { *kind = Mark::Hyphen; return 1; }
    if (i + 2 < s.size() && c == 0xEF && static_cast<unsigned char>(s[i + 1]) == 0xBC) {
        unsigned char d = static_cast<unsigned char>(s[i + 2]);
        if (d == 0x8E) { *kind = Mark::Period; return 3; }              // U+FF0E
        if (d == 0x89 || d == 0x9A) { *kind = Mark::Other; return 3; }  // U+FF09 U+FF1A
    }
    if (i + 2 < s.size() && c == 0xE3 && static_cast<unsigned char>(s[i + 1]) == 0x80) {
        unsigned char d = static_cast<unsigned char>(s[i + 2]);
        // U+3001 IDEOGRAPHIC COMMA, U+3002 IDEOGRAPHIC FULL STOP
        if (d == 0x81 || d == 0x82) { *kind = Mark::Other; return 3; }
    }
    return 0;
}

struct NumberedOpener {
    bool opens = false;     // the line begins with a line number
    bool unusable = false;  // ... in a shape that cannot be read as one
    int number = 0;
    std::string text;       // what follows the mark
};

// Try to parse "N. translation" from `line`.
NumberedOpener read_numbered_opener(const std::string& line) {
    NumberedOpener out;
    const std::size_t n = line.size();
    std::size_t i = 0;

    // Skip markdown decoration (emphasis, heading, indent) to find the number.
    std::size_t emphasis = 0;
    while (i < n) {
        if (line[i] == '*') {
            ++emphasis;
            ++i;
        } else if (line[i] == '#' || line[i] == '>' || is_blank(line[i])) {
            ++i;
        } else if (ideographic_space_at(line, i)) {
            i += IDEOGRAPHIC_SPACE_LEN;
        } else {
            break;
        }
    }

    int digits = 0;
    long long number = 0;
    for (;;) {
        int v = 0;
        std::size_t len = digit_at(line, i, &v);
        if (len == 0) break;
        if (++digits > MAX_LINE_NUMBER_DIGITS) return out;
        number = number * 10 + v;
        i += len;
    }
    if (digits == 0) return out;

    bool blank_before_mark = false;
    while (i < n && is_blank(line[i])) {  // "1 . Alpha"
        ++i;
        blank_before_mark = true;
    }
    Mark kind = Mark::None;
    std::size_t mark = mark_at(line, i, &kind);
    if (mark == 0) return out;  // "3 days later, ..." is prose
    const std::string mark_text = line.substr(i, mark);
    i += mark;
    // Consume repeated marks ("1..") so "1..5" still reads as prose.
    while (line.compare(i, mark, mark_text) == 0) i += mark;

    // Consume closing emphasis from "**2.** Bravo", up to what was opened before the number.
    std::size_t closed = 0;
    while (closed < emphasis && i < n && line[i] == '*') {
        ++i;
        ++closed;
    }

    // A digit after the mark means a time/decimal/range -- prose, not numbering.
    if (digit_at(line, i, nullptr) != 0) return out;

    // Number alone, hyphen mark, or spaced non-period mark: unusable numbering.
    // Reported rather than folded into the previous entry.
    if (i >= n || kind == Mark::Hyphen || (blank_before_mark && kind != Mark::Period)) {
        out.unusable = true;
        return out;
    }

    bool blank_after_mark = false;
    while (i < n && is_blank(line[i])) {
        ++i;
        blank_after_mark = true;
    }
    // Fullwidth space right after the mark is separator; after a blank it is a real indent.
    if (!blank_after_mark)
        while (ideographic_space_at(line, i)) i += IDEOGRAPHIC_SPACE_LEN;

    out.opens = true;
    out.number = static_cast<int>(number);
    out.text = line.substr(i);
    // Strip trailing emphasis that matches unclosed openers ("**1. Alpha**").
    for (std::size_t left = emphasis - closed;
         left > 0 && !out.text.empty() && out.text.back() == '*'; --left)
        out.text.pop_back();
    return out;
}

}  // namespace

std::map<int, std::string> parse_numbered_response(const std::string& response_text,
                                                   int expected, int* rejected_lines,
                                                   std::string* final_entry_tail) {
    std::map<int, std::string> translations;
    int current_num = -1;
    int highest_num = 0;  // numbers are 1-based, so 0 admits the first entry
    int rejected = 0;
    std::string current_head;               // the opener line's own text
    std::vector<std::string> current_tail;  // the lines that ran on after it
    std::string last_tail;

    auto flush = [&] {
        if (current_num < 0) return;
        std::string tail;
        for (const auto& part : current_tail) {
            if (!tail.empty()) tail += ' ';
            tail += part;
        }
        std::string joined = current_head;
        if (!tail.empty()) {
            if (!joined.empty()) joined += ' ';
            joined += tail;
        }
        last_tail = tail;
        // Unescape \n back to real breaks for the cache (what the game renders).
        translations[current_num] =
            strip(unescape_line_breaks(strip_speaker_tags(strip(joined))));
    };

    std::size_t pos = 0;
    while (pos <= response_text.size()) {
        std::size_t nl = response_text.find('\n', pos);
        std::string line = response_text.substr(
            pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? response_text.size() + 1 : nl + 1;

        line = strip(line);
        if (line.empty()) continue;

        NumberedOpener opener = read_numbered_opener(line);
        int num = 0;
        if (opener.opens) {
            // Out of order or past expected count: reject, don't fold in.
            if (opener.number <= highest_num || (expected > 0 && opener.number > expected))
                ++rejected;
            else
                num = opener.number;
        } else if (opener.unusable) {
            // Unreadable numbering -- reject so it doesn't silently glue to the previous entry.
            ++rejected;
        }

        if (num > 0) {
            flush();
            current_num = num;
            highest_num = num;
            current_head = strip(opener.text);
            current_tail.clear();
        } else if (current_num >= 0) {
            // Prose, so the whole line goes in -- decoration, digits and all.
            current_tail.push_back(line);
        }
    }
    flush();
    if (rejected_lines) *rejected_lines = rejected;
    if (final_entry_tail) *final_entry_tail = last_tail;
    // Any rejected line makes the whole answer unusable; re-queue the batch.
    if (rejected > 0) return {};
    return translations;
}

namespace {

// The answer was cut short by a token or context-window limit.
bool answer_was_cut(const std::string& stop_reason) {
    return stop_reason == "max_tokens" || stop_reason == "model_context_window_exceeded";
}

}  // namespace

bool answer_count_is_usable(std::size_t requested, std::size_t parsed,
                            const std::string& stop_reason) {
    if (answer_was_cut(stop_reason)) return true;
    return parsed == requested;
}

bool drop_truncated_entry(std::map<int, std::string>& translations,
                          const std::string& stop_reason) {
    if (!answer_was_cut(stop_reason)) return false;
    if (translations.empty()) return false;
    // The last entry is a fragment; drop it so it re-queues instead of caching half a sentence.
    translations.erase(std::prev(translations.end()));
    return true;
}

bool discard_refused_answer(std::map<int, std::string>& translations,
                            const std::string& stop_reason) {
    if (stop_reason != "refusal") return false;
    translations.clear();
    return true;
}

std::map<int, std::string> call_anthropic(
    anthropic::Client& client,
    const std::vector<std::pair<std::string, std::string>>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context) {
    bj::object body;
    body["model"] = MODEL;
    body["max_tokens"] = compute_max_tokens(static_cast<int>(batch_lines.size()));
    bj::object sys_block;
    sys_block["type"] = "text";
    sys_block["text"] = system_prompt();
    sys_block["cache_control"] = bj::object{{"type", "ephemeral"}};
    body["system"] = bj::array{std::move(sys_block)};
    bj::object msg;
    msg["role"] = "user";
    msg["content"] = build_user_prompt(batch_lines, previous_context);
    body["messages"] = bj::array{std::move(msg)};

    if (MODEL == MODEL_SONNET)
        body["temperature"] = 0.3;
    // reasoning_control() decides the reasoning mode; wrong model = HTTP 400.
    switch (reasoning_control(MODEL)) {
    case ReasoningControl::AdaptiveThinking:
        body["thinking"] = bj::object{{"type", "adaptive"}};
        break;
    case ReasoningControl::Effort:
        body["output_config"] = bj::object{{"effort", EFFORT}};
        break;
    case ReasoningControl::None:
        break;
    }

    bj::value response = client.messages(bj::value(std::move(body)));

    // Skip thinking blocks; extract the first "text" block.
    std::string response_text;
    if (auto* content = response.get_object().if_contains("content"))
        for (const auto& block : content->get_array())
            if (auto* t = block.get_object().if_contains("type"))
                if (t->is_string() && t->get_string() == "text") {
                    if (auto* txt = block.get_object().if_contains("text"))
                        response_text = strip(std::string(txt->get_string()));
                    break;
                }

    // "max_tokens" means the last entry is a truncated fragment.
    std::string stop_reason;
    if (auto* sr = response.get_object().if_contains("stop_reason"))
        if (sr->is_string()) stop_reason = std::string(sr->get_string());

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
    int rejected = 0;
    std::string final_tail;
    auto translations = parse_numbered_response(
        response_text, static_cast<int>(batch_lines.size()), &rejected, &final_tail);
    const std::size_t parsed_entries = translations.size();
    if (!final_tail.empty()) ++trailing_text_events();

    std::string summary =
        "lines=" + std::to_string(batch_lines.size()) +
        " stop_reason=" + (stop_reason.empty() ? std::string("(absent)") : stop_reason) +
        " in=" + std::to_string(in_tok) + " out=" + std::to_string(out_tok) +
        " cache_read=" + std::to_string(cache_read) +
        " cache_create=" + std::to_string(cache_create) +
        " last_entry_continued=" + (final_tail.empty() ? "0" : "1");

    // Console gets a preview; the side file keeps every answer whole.
    append_raw_answer(summary, response_text);

    log_info("--- CLAUDE RAW ANSWER (" + summary + ") ---");
    log_info(response_text.substr(0, RAW_ANSWER_PREVIEW));
    if (response_text.size() > RAW_ANSWER_PREVIEW && !raw_answer_log().empty())
        log_info("  [preview cut -- full answer appended to " + raw_answer_log() + "]");
    log_info("-------------------------");

    if (!stop_reason.empty() && stop_reason != "end_turn")
        log_info("    WARNING: the model stopped on " + stop_reason + ", not end_turn");

    // Trailing text looks like a two-line translation; report rather than reject.
    if (!final_tail.empty()) {
        std::string preview = final_tail.substr(0, TRAILING_TEXT_PREVIEW);
        if (final_tail.size() > TRAILING_TEXT_PREVIEW) preview += " [...]";
        log_info("    NOTE: the last entry ran on past its own line (" +
                 std::to_string(trailing_text_events()) +
                 " so far this run) -- text joined onto it: " + preview);
        append_raw_answer("text joined onto the last entry", final_tail);
    }

    if (rejected > 0)
        log_info("    WARNING: " + std::to_string(rejected) +
                 " unusable numbered line(s) -- the answer's numbering cannot be "
                 "trusted, so the whole batch is discarded and re-queues");
    if (discard_refused_answer(translations, stop_reason))
        log_info("    WARNING: the model refused -- the answer stops wherever it had "
                 "got to, so the whole batch is discarded and re-queues");
    else if (drop_truncated_entry(translations, stop_reason))
        log_info("    WARNING: answer cut at " + stop_reason +
                 " -- dropped the last entry, which is a fragment, so that line "
                 "re-queues");
    // Count mismatch on a voluntary stop means numbering shifted; discard all.
    else if (rejected == 0 &&
             !answer_count_is_usable(batch_lines.size(), parsed_entries, stop_reason)) {
        translations.clear();
        log_info("    WARNING: the answer holds " + std::to_string(parsed_entries) +
                 " translations for " + std::to_string(batch_lines.size()) +
                 " lines and stopped on its own, so its numbering cannot be lined up "
                 "with the batch -- the whole batch is discarded and re-queues");
    }

    return translations;
}

// --------------------------------------------------------------------------- //
// Per-script translation
// --------------------------------------------------------------------------- //

namespace {

// Returns true when the batch budget ran out before the script was finished.
bool translate_script(anthropic::LazyClient& client, const bj::object& script,
                      Cache& cache, int batch_size,
                      const std::function<void(const Cache&)>& save_fn,
                      bool test_mode, int* batches_remaining) {
    const auto* strings_v = script.if_contains("strings");
    if (!strings_v || !strings_v->is_array()) return false;
    const auto& entries = strings_v->get_array();
    if (entries.empty()) return false;

    std::vector<std::pair<std::string, std::string>> previous_context;  // (speaker, en)
    bool stopped_early = false;

    for (std::size_t batch_start = 0; batch_start < entries.size();
         batch_start += static_cast<std::size_t>(batch_size)) {
        std::size_t batch_end =
            std::min(entries.size(), batch_start + static_cast<std::size_t>(batch_size));

        std::vector<std::pair<std::string, std::string>> needs_api;  // (speaker, jp)
        for (std::size_t i = batch_start; i < batch_end; ++i) {
            const auto& entry = entries[i].get_object();
            std::string jp(entry.at("text").get_string());
            std::string speaker = extract_speaker(entry);
            if (const std::string* en = cache.get(jp))
                previous_context.emplace_back(speaker, *en);
            else
                needs_api.emplace_back(speaker, jp);
        }

        if (needs_api.empty()) continue;

        // The first batch the cache cannot cover is where a key becomes
        // required.  Outside the try below on purpose: a missing key has to
        // stop the run, not be caught as a batch error and cached as a
        // failed line.
        anthropic::Client& api = client.get();

        print_context_window(previous_context);

        try {
            auto translations = call_anthropic(api, needs_api, previous_context);
            if (translations.size() < needs_api.size())
                log_info("    WARN: got " + std::to_string(translations.size()) + "/" +
                         std::to_string(needs_api.size()) + " translations");

            for (std::size_t i = 0; i < needs_api.size(); ++i) {
                const auto& [speaker, jp] = needs_api[i];
                std::string en;
                auto it = translations.find(static_cast<int>(i) + 1);
                if (it != translations.end()) en = strip(it->second);
                if (en.empty())
                    log_info("    Missing translation for line " + std::to_string(i + 1) +
                             " -- leaving it queued");
                apply_line_result(cache, previous_context, speaker, jp, en);
            }
        } catch (const anthropic::RateLimitError&) {
            log_info("    Rate limited, waiting 60s...");
            std::this_thread::sleep_for(std::chrono::seconds(60));
            try {
                auto translations = call_anthropic(api, needs_api, previous_context);
                for (std::size_t i = 0; i < needs_api.size(); ++i) {
                    const auto& [speaker, jp] = needs_api[i];
                    std::string en;
                    auto it = translations.find(static_cast<int>(i) + 1);
                    if (it != translations.end()) en = strip(it->second);
                    apply_line_result(cache, previous_context, speaker, jp, en);
                }
            } catch (const std::exception& e2) {
                log_info(std::string("    Retry failed: ") + e2.what());
                for (const auto& [speaker, jp] : needs_api)
                    apply_line_result(cache, previous_context, speaker, jp, "");
            }
        } catch (const std::exception& e) {
            log_info(std::string("    Batch error: ") + e.what());
            for (const auto& [speaker, jp] : needs_api)
                apply_line_result(cache, previous_context, speaker, jp, "");
        }

        if (previous_context.size() > CONTEXT_WINDOW * 2)
            previous_context.erase(
                previous_context.begin(),
                previous_context.end() - CONTEXT_WINDOW * 2);

        if (save_fn) save_fn(cache);
        if (test_mode && batches_remaining) {
            --*batches_remaining;
            if (*batches_remaining <= 0) {
                stopped_early = true;
                break;
            }
        }
    }

    return stopped_early;
}

}  // namespace

// --------------------------------------------------------------------------- //
// Driver
// --------------------------------------------------------------------------- //

int translate_all(const TranslateOptions& opt) {
    log_info("Model: " + MODEL);
    log_info("Reasoning: " + reasoning_control_label(MODEL));
    log_info("Batch size: " + std::to_string(opt.batch_size) +
             ", context window: " + std::to_string(CONTEXT_WINDOW) +
             ", max_tokens: " + std::to_string(compute_max_tokens(opt.batch_size)));

    log_info("\nLoading extracted text...");
    bj::value all_scripts_v = json_parse_file(opt.input_file);
    bj::object& all_scripts = all_scripts_v.get_object();

    // Gate first: a run that will fail must cost nothing and leave nothing behind.
    log_info("\n--- Speaker gate (no tokens spent) ---");
    SpeakerCheckReport gate = run_speaker_checks(all_scripts);
    if (!gate.passed) {
        log_info("\n[ERROR] speaker check " + std::to_string(gate.failed_check) +
                 " failed: " + gate.reason);
        log_info("        Nothing was translated and nothing was written.");
        return 2;
    }

    // Fresh run: drop earlier output so this run's table starts clean.
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

    set_raw_answer_log(raw_answer_log_path(opt.output_file));
    log_info("Raw answers: " + raw_answer_log());

    anthropic::LazyClient client;

    log_info("Loading translation cache...");
    Cache cache;
    if (opt.retranslate) {
        log_info("  --retranslate: starting fresh cache");
    } else {
        cache = load_cache(opt.cache_file);
        log_info("  Cache has " + std::to_string(cache.size()) + " entries");
        std::size_t purged = purge_failed_entries(cache);
        log_info("  Failed entries purged: " + std::to_string(purged) +
                 (purged ? "  (those lines re-queue)" : ""));
    }

    // Seed with curated name translations.
    for (const auto& [jp, en] : name_translations_ordered())
        cache.set_default(jp, en);

    // (script_name, total_in_script, uncached), iterated in sorted order.
    std::vector<std::tuple<std::string, std::int64_t, std::int64_t>> scripts_with_text;
    std::int64_t total_uncached = 0;
    {
        std::vector<std::string> names;
        for (const auto& kv : all_scripts) names.emplace_back(kv.key());
        std::sort(names.begin(), names.end());
        for (const auto& script_name : names) {
            if (opt.only_script && script_name != *opt.only_script) continue;
            const auto& script = all_scripts.at(script_name).get_object();
            const auto* strings_v = script.if_contains("strings");
            if (!strings_v || !strings_v->is_array() || strings_v->get_array().empty())
                continue;
            std::int64_t uncached = 0;
            for (const auto& ev : strings_v->get_array())
                if (!cache.contains(std::string(ev.get_object().at("text").get_string())))
                    ++uncached;
            std::int64_t total = 0;
            if (auto* j = script.if_contains("jp_strings"))
                if (j->is_int64()) total = j->get_int64();
            scripts_with_text.emplace_back(script_name, total, uncached);
            total_uncached += uncached;
        }
    }

    log_info("\nScripts: " + std::to_string(scripts_with_text.size()) +
             ", uncached strings: " + std::to_string(total_uncached));

    int batches_remaining = opt.test_mode ? opt.num_batches : 0;

    if (total_uncached == 0) {
        log_info("  Everything is already cached!");
    } else {
        log_info("\n--- Translating in script order ---");
        std::int64_t done_lines = 0;
        std::int64_t done_scripts = 0;
        auto start = std::chrono::steady_clock::now();

        for (const auto& [script_name, total_in_script, uncached] : scripts_with_text) {
            if (uncached == 0) {
                done_lines += total_in_script;
                ++done_scripts;
                continue;
            }

            log_info("  [" + std::to_string(done_scripts + 1) + "/" +
                     std::to_string(scripts_with_text.size()) + "] " + script_name +
                     "  (" + std::to_string(uncached) + "/" +
                     std::to_string(total_in_script) + " uncached)");

            bool stopped = translate_script(
                client, all_scripts.at(script_name).get_object(), cache, opt.batch_size,
                [&](const Cache& c) { save_cache(c, opt.cache_file); },
                opt.test_mode, opt.test_mode ? &batches_remaining : nullptr);
            done_lines += total_in_script;
            ++done_scripts;

            double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
                    .count();
            double rate = elapsed > 0 ? static_cast<double>(done_lines) / elapsed : 0.0;
            char buf[64];
            std::snprintf(buf, sizeof(buf), "    %.1f lines/s  done=%lld", rate,
                          static_cast<long long>(done_lines));
            log_info(buf);

            if (stopped) {
                log_info("\n  Batch budget spent -- stopping");
                break;
            }
        }

        save_cache(cache, opt.cache_file);
        log_info("\nTranslation pass done. Cache: " + std::to_string(cache.size()) +
                 " entries");
    }

    // Materialise translated_text.json
    log_info("\nWriting translated_text.json...");
    for (auto& kv : all_scripts) {
        auto& script = kv.value().get_object();
        auto* strings_v = script.if_contains("strings");
        if (!strings_v || !strings_v->is_array()) continue;
        for (auto& ev : strings_v->get_array()) {
            auto& entry = ev.get_object();
            std::string jp(entry.at("text").get_string());
            if (const std::string* en = cache.get(jp))
                entry["translated"] = *en;
            else
                entry["translated"] = jp;  // non-JP passthrough / fallback
        }
    }

    write_file(opt.output_file, bj::serialize(all_scripts_v));
    char sz[64];
    std::snprintf(sz, sizeof(sz), "%.1f MB",
                  static_cast<double>(fs::file_size(fs::u8path(opt.output_file))) /
                      1024.0 / 1024.0);
    log_info("  -> " + opt.output_file + " (" + sz + ")");

    // Build the runtime TSV the game loads, plus the coverage report.
    print_line("");
    log_info("Building the runtime translation table...");
    return build_tsv::run_build(opt.output_file, opt.tsv_file, opt.include_untranslated);
}

}  // namespace snu::translate
