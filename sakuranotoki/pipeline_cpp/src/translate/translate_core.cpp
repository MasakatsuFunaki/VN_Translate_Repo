// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "translate_core.h"

#include "build_tsv/build_tsv.h"
#include "common/coverage.h"
#include "common/util.h"
#include "glossary.h"
#include "speaker_gate.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/regex.hpp>

namespace snt::translate {

namespace bj = boost::json;
namespace fs = std::filesystem;

bool needs_translation(const std::string& text) {
    if (text.empty()) {
        return false;
    }
    return is_jp(text);
}

std::string strip(const std::string& s) { return trim(s); }

std::optional<std::string> validate_options(int batch_size, int test_n, int max_batches) {
    if (batch_size <= 0) {
        return "--batch must be at least 1 (got " + std::to_string(batch_size) + ")";
    }
    if (test_n < 0) {
        return "--test must not be negative (got " + std::to_string(test_n) + ")";
    }
    if (max_batches < 0) {
        return "--max-batches must not be negative (got " +
               std::to_string(max_batches) + ")";
    }
    return std::nullopt;
}

namespace {

constexpr std::int64_t kMissingOffset = -1;
constexpr int kPrettyIndentSpaces = 2;
constexpr unsigned char kNonAsciiThreshold = 0x80U;
constexpr std::size_t kTrailingTextPreview = 300;
constexpr std::size_t kMaxContextPreviewChars = 120;
constexpr std::size_t kCrlfSeqLen = 2;
constexpr std::size_t kContextTrimMultiplier = 2;
constexpr int kExitOk = 0;
constexpr int kExitFailure = 1;
constexpr int kExitGateFailed = 2;
constexpr std::size_t kStatsBufChars = 64;
constexpr double kBytesPerKilobyte = 1024.0;
constexpr double kBytesPerMegabyte = kBytesPerKilobyte * kBytesPerKilobyte;

// U+3000 IDEOGRAPHIC SPACE.
constexpr std::string_view kIdeographicSpace = "　";
constexpr int kMaxLineNumberDigits = 9;
constexpr long long kDecimalRadix = 10;
constexpr std::size_t kAsciiCharBytes = 1;
constexpr std::size_t kUtf8ThreeByteChars = 3;
constexpr std::size_t kUtf8ThirdByteOffset = 2;
constexpr unsigned char kFullwidthLead0 = 0xEFU;
constexpr unsigned char kFullwidthLead1 = 0xBCU;
constexpr unsigned char kFullwidthDigitZeroTrail = 0x90U;
constexpr unsigned char kFullwidthDigitNineTrail = 0x99U;
constexpr unsigned char kFullwidthPeriodTrail = 0x8EU;
constexpr unsigned char kFullwidthCloseParenTrail = 0x89U;
constexpr unsigned char kFullwidthColonTrail = 0x9AU;
constexpr unsigned char kCjkPunctLead0 = 0xE3U;
constexpr unsigned char kCjkPunctLead1 = 0x80U;
constexpr unsigned char kIdeographicCommaTrail = 0x81U;
constexpr unsigned char kIdeographicPeriodTrail = 0x82U;
constexpr int kNoCurrentEntry = -1;

bool is_ascii_blank(char c) { return c == ' ' || c == '\t'; }

std::string entry_type(const bj::object& entry) {
    if (const auto* t = entry.if_contains("type");
        t != nullptr && t->is_string()) {
        return std::string(t->get_string());
    }
    return {};
}

std::int64_t entry_offset(const bj::object& entry) {
    if (const auto* o = entry.if_contains("offset");
        o != nullptr && o->is_int64()) {
        return o->get_int64();
    }
    return kMissingOffset;
}

std::optional<std::string> speaker_from_field(const bj::object& entry) {
    const auto* s = entry.if_contains("speaker");
    if (s == nullptr) {
        return std::nullopt;
    }
    if (s->is_null()) {
        return std::string("NARRATION");
    }
    if (!s->is_string()) {
        return std::nullopt;
    }
    const std::string raw = strip(std::string(s->get_string()));
    if (raw.empty()) {
        return std::string("NARRATION");
    }
    const auto& map = name_translations();
    const auto it = map.find(raw);
    return it == map.end() ? raw : it->second;
}

std::string& raw_answer_log_slot() {
    static std::string path;
    return path;
}

std::size_t& trailing_text_events() {
    static std::size_t count = 0;
    return count;
}

void append_raw_answer(const std::string& summary, const std::string& answer) {
    const std::string& path = raw_answer_log_slot();
    if (path.empty()) {
        return;
    }
    std::ofstream f(fs::u8path(path), std::ios::binary | std::ios::app);
    if (!f) {
        return;
    }
    f << "=== " << summary << " ===\n" << answer << "\n\n";
}

void print_context_window(
    const std::vector<std::pair<std::string, std::string>>& previous_context) {
    const std::size_t n =
        std::min<std::size_t>(previous_context.size(), CONTEXT_WINDOW);
    log_info("\n=== ROLLING CONTEXT (last " + std::to_string(n) + " of " +
             std::to_string(previous_context.size()) + ") ===");
    if (n == 0) {
        log_info("  (empty -- start of script)");
        log_info("=== END CONTEXT ===\n");
        return;
    }
    for (std::size_t i = previous_context.size() - n;
         i < previous_context.size(); ++i) {
        const auto& [speaker, txt] = previous_context[i];
        std::string short_txt;
        for (std::size_t k = 0; k < txt.size(); ++k) {
            if (txt.compare(k, kCrlfSeqLen, "\r\n") == 0) {
                short_txt += '|';
                ++k;
                continue;
            }
            if (txt[k] == '\n') {
                short_txt += '|';
                continue;
            }
            short_txt += txt[k];
        }
        if (short_txt.size() > kMaxContextPreviewChars) {
            short_txt.resize(kMaxContextPreviewChars);
        }
        log_info("  [" + speaker + "] " + short_txt);
    }
    log_info("=== END CONTEXT ===\n");
}

bool is_blank(char c) { return c == ' ' || c == '\t'; }

bool ideographic_space_at(const std::string& s, std::size_t i) {
    return s.compare(i, kIdeographicSpace.size(), kIdeographicSpace.data()) == 0;
}

std::size_t digit_at(const std::string& s, std::size_t i, int* value) {
    if (i >= s.size()) {
        return 0;
    }
    const auto c = static_cast<unsigned char>(s[i]);
    if (c >= '0' && c <= '9') {
        if (value != nullptr) {
            *value = c - '0';
        }
        return kAsciiCharBytes;
    }
    if (i + kUtf8ThirdByteOffset < s.size() && c == kFullwidthLead0 &&
        static_cast<unsigned char>(s[i + 1]) == kFullwidthLead1) {
        const auto d = static_cast<unsigned char>(s[i + kUtf8ThirdByteOffset]);
        if (d >= kFullwidthDigitZeroTrail && d <= kFullwidthDigitNineTrail) {
            if (value != nullptr) {
                *value = d - kFullwidthDigitZeroTrail;
            }
            return kUtf8ThreeByteChars;
        }
    }
    return 0;
}

enum class Mark {
    None,
    Period,
    Other,
    Hyphen,
};

std::size_t mark_at(const std::string& s, std::size_t i, Mark* kind) {
    *kind = Mark::None;
    if (i >= s.size()) {
        return 0;
    }
    const auto c = static_cast<unsigned char>(s[i]);
    if (c == '.') {
        *kind = Mark::Period;
        return kAsciiCharBytes;
    }
    if (c == ')' || c == ':') {
        *kind = Mark::Other;
        return kAsciiCharBytes;
    }
    if (c == '-') {
        *kind = Mark::Hyphen;
        return kAsciiCharBytes;
    }
    if (i + kUtf8ThirdByteOffset < s.size() && c == kFullwidthLead0 &&
        static_cast<unsigned char>(s[i + 1]) == kFullwidthLead1) {
        const auto d = static_cast<unsigned char>(s[i + kUtf8ThirdByteOffset]);
        if (d == kFullwidthPeriodTrail) {
            *kind = Mark::Period;
            return kUtf8ThreeByteChars;
        }
        if (d == kFullwidthCloseParenTrail || d == kFullwidthColonTrail) {
            *kind = Mark::Other;
            return kUtf8ThreeByteChars;
        }
    }
    if (i + kUtf8ThirdByteOffset < s.size() && c == kCjkPunctLead0 &&
        static_cast<unsigned char>(s[i + 1]) == kCjkPunctLead1) {
        const auto d = static_cast<unsigned char>(s[i + kUtf8ThirdByteOffset]);
        if (d == kIdeographicCommaTrail || d == kIdeographicPeriodTrail) {
            *kind = Mark::Other;
            return kUtf8ThreeByteChars;
        }
    }
    return 0;
}

struct NumberedOpener {
    bool opens = false;
    bool unusable = false;
    int number = 0;
    std::string text;
};

NumberedOpener read_numbered_opener(const std::string& line) {
    NumberedOpener out;
    const std::size_t n = line.size();
    std::size_t i = 0;

    std::size_t emphasis = 0;
    while (i < n) {
        if (line[i] == '*') {
            ++emphasis;
            ++i;
            continue;
        }
        if (line[i] == '#' || line[i] == '>' || is_blank(line[i])) {
            ++i;
            continue;
        }
        if (ideographic_space_at(line, i)) {
            i += kIdeographicSpace.size();
            continue;
        }
        break;
    }

    int digits = 0;
    long long number = 0;
    for (;;) {
        int v = 0;
        const std::size_t len = digit_at(line, i, &v);
        if (len == 0) {
            break;
        }
        if (++digits > kMaxLineNumberDigits) {
            return out;
        }
        number = number * kDecimalRadix + v;
        i += len;
    }
    if (digits == 0) {
        return out;
    }

    bool blank_before_mark = false;
    while (i < n && is_blank(line[i])) {
        ++i;
        blank_before_mark = true;
    }
    Mark kind = Mark::None;
    const std::size_t mark = mark_at(line, i, &kind);
    if (mark == 0) {
        return out;
    }
    const std::string mark_text = line.substr(i, mark);
    i += mark;
    while (line.compare(i, mark, mark_text) == 0) {
        i += mark;
    }

    std::size_t closed = 0;
    while (closed < emphasis && i < n && line[i] == '*') {
        ++i;
        ++closed;
    }

    if (digit_at(line, i, nullptr) != 0) {
        return out;
    }

    if (i >= n || kind == Mark::Hyphen ||
        (blank_before_mark && kind != Mark::Period)) {
        out.unusable = true;
        return out;
    }

    bool blank_after_mark = false;
    while (i < n && is_blank(line[i])) {
        ++i;
        blank_after_mark = true;
    }
    if (!blank_after_mark) {
        while (ideographic_space_at(line, i)) {
            i += kIdeographicSpace.size();
        }
    }

    out.opens = true;
    out.number = static_cast<int>(number);
    out.text = line.substr(i);
    for (std::size_t left = emphasis - closed;
         left > 0 && !out.text.empty() && out.text.back() == '*'; --left) {
        out.text.pop_back();
    }
    return out;
}

bool answer_was_cut(const std::string& stop_reason) {
    return stop_reason == "max_tokens" ||
           stop_reason == "model_context_window_exceeded";
}

bool translate_script(gemini::LazyClient& client, const bj::object& script,
                      Cache& cache, int batch_size,
                      const std::function<void(const Cache&)>& save_fn,
                      bool test_mode, int* batches_remaining) {
    const auto* strings_v = script.if_contains("strings");
    if (strings_v == nullptr || !strings_v->is_array()) {
        return false;
    }
    const auto& entries = strings_v->get_array();
    if (entries.empty()) {
        return false;
    }

    SpeakerSource source;
    const auto annotated = iter_entries_with_speakers(entries, &source);
    if (source.disagreements > 0) {
        log_info("    speaker field answered " + std::to_string(source.disagreements) +
                 " of " + std::to_string(source.from_field) +
                 " entries the state machine reads differently (first at offset " +
                 std::to_string(source.first_disagreement_offset) + ")");
    }

    std::vector<std::pair<std::string, std::string>> previous_context;
    bool stopped_early = false;

    for (std::size_t batch_start = 0; batch_start < annotated.size();
         batch_start += static_cast<std::size_t>(batch_size)) {
        const std::size_t batch_end = std::min(
            annotated.size(), batch_start + static_cast<std::size_t>(batch_size));

        std::vector<std::pair<std::string, std::string>> needs_api;
        for (std::size_t i = batch_start; i < batch_end; ++i) {
            const auto& [speaker, entry_ptr] = annotated[i];
            const auto& entry = *entry_ptr;
            std::string jp(entry.at("text").get_string());
            if (const std::string* en = cache.get(jp)) {
                previous_context.emplace_back(speaker, *en);
            } else {
                needs_api.emplace_back(speaker, jp);
            }
        }

        if (needs_api.empty()) {
            continue;
        }

        gemini::Client& api = client.get();
        print_context_window(previous_context);

        translate_batch(cache, previous_context, needs_api, [&] {
            return call_gemini(api, needs_api, previous_context);
        });

        const std::size_t max_retained_context =
            CONTEXT_WINDOW * kContextTrimMultiplier;
        if (previous_context.size() > max_retained_context) {
            previous_context.erase(
                previous_context.begin(),
                previous_context.end() -
                    static_cast<std::ptrdiff_t>(max_retained_context));
        }

        if (save_fn) {
            save_fn(cache);
        }
        if (test_mode && batches_remaining != nullptr) {
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

std::string unescape_line_breaks(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size() && s[i + 1] == 'n') {
            while (!out.empty() && is_ascii_blank(out.back())) {
                out.pop_back();
            }
            out += '\n';
            ++i;
            while (i + 1 < s.size() && is_ascii_blank(s[i + 1])) {
                ++i;
            }
            continue;
        }
        out += s[i];
    }
    return out;
}

std::string strip_speaker_tags(const std::string& text) {
    static const boost::regex re(
        R"(^\[(?:NARRATION|[A-Za-z0-9\s()'\"?&$.,!-]+)\]\s*)");
    return strip(boost::regex_replace(text, re, ""));
}

std::int64_t compute_max_tokens(int batch_size) {
    const std::int64_t want =
        static_cast<std::int64_t>(std::max(batch_size, 0)) * OUTPUT_TOKENS_PER_LINE;
    return std::clamp(want, MIN_OUTPUT_TOKENS, MAX_OUTPUT_TOKENS);
}

std::string extract_speaker(const bj::object& entry, const std::string& current_speaker) {
    const std::string t = entry_type(entry);
    if (t == "name") {
        return "NAME";
    }
    if (t == "title") {
        return "TITLE";
    }
    if (t == "dialogue") {
        return current_speaker.empty() ? "NARRATION" : current_speaker;
    }
    return "NARRATION";
}

std::string update_speaker(const bj::object& entry, const std::string& current_speaker) {
    const std::string t = entry_type(entry);
    if (t == "title") {
        return {};
    }
    if (t == "name") {
        std::string name;
        if (const auto* s = entry.if_contains("text");
            s != nullptr && s->is_string()) {
            name = strip(std::string(s->get_string()));
        }
        if (!name.empty()) {
            const auto& map = name_translations();
            const auto it = map.find(name);
            return it == map.end() ? name : it->second;
        }
    }
    return current_speaker;
}

std::vector<std::pair<std::string, const bj::object*>> iter_entries_with_speakers(
    const bj::array& entries, SpeakerSource* source) {
    std::vector<std::pair<std::string, const bj::object*>> out;
    out.reserve(entries.size());
    std::string current_speaker;
    for (const auto& ev : entries) {
        const bj::object& entry = ev.get_object();
        std::string tag = extract_speaker(entry, current_speaker);
        if (auto field = speaker_from_field(entry)) {
            if (source != nullptr) {
                ++source->from_field;
                if (*field != tag) {
                    ++source->disagreements;
                    if (source->first_disagreement_offset < 0) {
                        source->first_disagreement_offset = entry_offset(entry);
                    }
                }
            }
            tag = std::move(*field);
        }
        out.emplace_back(std::move(tag), &entry);
        current_speaker = update_speaker(entry, current_speaker);
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

void save_cache(const Cache& cache, const std::string& cache_file) {
    bj::object o;
    for (const auto& [jp, en] : cache.items()) {
        o[jp] = en;
    }
    write_file_atomic_text(
        cache_file, json_pretty(bj::value(std::move(o)), kPrettyIndentSpaces));
}

bool is_failed_entry(const std::string& jp, const std::string& en) {
    if (jp != en) {
        return false;
    }
    for (char c : jp) {
        if (static_cast<unsigned char>(c) >= kNonAsciiThreshold) {
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

void apply_line_result(Cache& cache,
                       std::vector<std::pair<std::string, std::string>>& previous_context,
                       const std::string& speaker, const std::string& jp,
                       const std::string& en) {
    if (en.empty()) {
        previous_context.emplace_back(speaker, jp);
        return;
    }
    cache.set(jp, en);
    previous_context.emplace_back(speaker, en);
}

void set_raw_answer_log(const std::string& path) { raw_answer_log_slot() = path; }

const std::string& raw_answer_log() { return raw_answer_log_slot(); }

std::string raw_answer_log_path(const std::string& sibling_file) {
    const fs::path p = fs::u8path(sibling_file);
    return (p.parent_path() / fs::u8path("gemini_raw_answers.log")).u8string();
}

std::string build_user_prompt(
    const std::vector<std::pair<std::string, std::string>>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context) {
    std::string user_prompt;
    if (!previous_context.empty()) {
        user_prompt += "<previous_context>\n";
        const std::size_t start = previous_context.size() > CONTEXT_WINDOW
                                      ? previous_context.size() - CONTEXT_WINDOW
                                      : 0;
        for (std::size_t i = start; i < previous_context.size(); ++i) {
            user_prompt += "[" + previous_context[i].first + "] " +
                           previous_context[i].second + "\n";
        }
        user_prompt += "</previous_context>\n\n";
    }
    user_prompt += "<lines_to_translate>\n";
    for (std::size_t i = 0; i < batch_lines.size(); ++i) {
        user_prompt += std::to_string(i + 1) + ". [" + batch_lines[i].first + "] " +
                       batch_lines[i].second + "\n";
    }
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

std::map<int, std::string> parse_numbered_response(const std::string& response_text,
                                                   int expected, int* rejected_lines,
                                                   std::string* final_entry_tail) {
    std::map<int, std::string> translations;
    int current_num = kNoCurrentEntry;
    int highest_num = 0;
    int rejected = 0;
    std::string current_head;
    std::vector<std::string> current_tail;
    std::string last_tail;

    auto flush = [&] {
        if (current_num < 0) {
            return;
        }
        std::string tail;
        for (const auto& part : current_tail) {
            if (!tail.empty()) {
                tail += ' ';
            }
            tail += part;
        }
        std::string joined = current_head;
        if (!tail.empty()) {
            if (!joined.empty()) {
                joined += ' ';
            }
            joined += tail;
        }
        last_tail = tail;
        translations[current_num] =
            strip(unescape_line_breaks(strip_speaker_tags(strip(joined))));
    };

    std::size_t pos = 0;
    while (pos <= response_text.size()) {
        const std::size_t nl = response_text.find('\n', pos);
        std::string line = response_text.substr(
            pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? response_text.size() + 1 : nl + 1;

        line = strip(line);
        if (line.empty()) {
            continue;
        }

        const NumberedOpener opener = read_numbered_opener(line);
        int num = 0;
        if (opener.opens) {
            if (opener.number <= highest_num ||
                (expected > 0 && opener.number > expected)) {
                ++rejected;
            } else {
                num = opener.number;
            }
        } else if (opener.unusable) {
            ++rejected;
        }

        if (num > 0) {
            flush();
            current_num = num;
            highest_num = num;
            current_head = strip(opener.text);
            current_tail.clear();
            continue;
        }
        if (current_num >= 0) {
            current_tail.push_back(line);
        }
    }
    flush();
    if (rejected_lines != nullptr) {
        *rejected_lines = rejected;
    }
    if (final_entry_tail != nullptr) {
        *final_entry_tail = last_tail;
    }
    if (rejected > 0) {
        return {};
    }
    return translations;
}

bool answer_count_is_usable(std::size_t requested, std::size_t parsed,
                            const std::string& stop_reason) {
    if (answer_was_cut(stop_reason)) {
        return true;
    }
    return parsed == requested;
}

bool drop_truncated_entry(std::map<int, std::string>& translations,
                          const std::string& stop_reason) {
    if (!answer_was_cut(stop_reason) || translations.empty()) {
        return false;
    }
    translations.erase(std::prev(translations.end()));
    return true;
}

bool discard_refused_answer(std::map<int, std::string>& translations,
                            const std::string& stop_reason) {
    if (stop_reason != "refusal") {
        return false;
    }
    translations.clear();
    return true;
}

Answer read_answer(const bj::value& response) {
    Answer answer;
    if (!response.is_object()) {
        return answer;
    }
    const bj::object& root = response.get_object();

    if (const auto* content = root.if_contains("content");
        content != nullptr && content->is_array()) {
        for (const auto& block : content->get_array()) {
            if (!block.is_object()) {
                continue;
            }
            const auto* t = block.get_object().if_contains("type");
            if (t == nullptr || !t->is_string() || t->get_string() != "text") {
                continue;
            }
            if (const auto* txt = block.get_object().if_contains("text");
                txt != nullptr && txt->is_string()) {
                answer.text = strip(std::string(txt->get_string()));
            }
            break;
        }
    }

    if (const auto* sr = root.if_contains("stop_reason");
        sr != nullptr && sr->is_string()) {
        answer.stop_reason = std::string(sr->get_string());
    }
    return answer;
}

std::map<int, std::string> usable_translations(const Answer& answer, std::size_t requested) {
    int rejected = 0;
    std::string final_tail;
    auto translations = parse_numbered_response(
        answer.text, static_cast<int>(requested), &rejected, &final_tail);
    const std::size_t parsed_entries = translations.size();

    if (!answer.stop_reason.empty() && answer.stop_reason != "end_turn") {
        log_info("    WARNING: the model stopped on " + answer.stop_reason +
                 ", not end_turn");
    }

    if (!final_tail.empty()) {
        ++trailing_text_events();
        std::string preview = final_tail.substr(0, kTrailingTextPreview);
        if (final_tail.size() > kTrailingTextPreview) {
            preview += " [...]";
        }
        log_info("    NOTE: the last entry ran on past its own line (" +
                 std::to_string(trailing_text_events()) +
                 " so far this run) -- text joined onto it: " + preview);
        append_raw_answer("text joined onto the last entry", final_tail);
    }

    if (rejected > 0) {
        log_info("    WARNING: " + std::to_string(rejected) +
                 " unusable numbered line(s) -- the answer's numbering cannot be "
                 "trusted, so the whole batch is discarded and re-queues");
    }
    if (discard_refused_answer(translations, answer.stop_reason)) {
        log_info("    WARNING: the model refused -- the answer stops wherever it had "
                 "got to, so the whole batch is discarded and re-queues");
        return translations;
    }
    if (drop_truncated_entry(translations, answer.stop_reason)) {
        log_info("    WARNING: answer cut at " + answer.stop_reason +
                 " -- dropped the last entry, which is a fragment, so that line "
                 "re-queues");
        return translations;
    }
    if (rejected == 0 &&
        !answer_count_is_usable(requested, parsed_entries, answer.stop_reason)) {
        translations.clear();
        log_info("    WARNING: the answer holds " + std::to_string(parsed_entries) +
                 " translations for " + std::to_string(requested) +
                 " lines and stopped on its own, so its numbering cannot be lined up "
                 "with the batch -- the whole batch is discarded and re-queues");
    }
    return translations;
}

std::map<int, std::string> call_gemini(
    gemini::Client& client,
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

    const bj::value response = client.messages(bj::value(std::move(body)));
    const Answer answer = read_answer(response);

    std::int64_t in_tok = 0;
    std::int64_t out_tok = 0;
    std::int64_t cache_read = 0;
    std::int64_t cache_create = 0;
    if (const auto* usage = response.get_object().if_contains("usage");
        usage != nullptr && usage->is_object()) {
        const auto& u = usage->get_object();
        auto geti = [&](const char* k) -> std::int64_t {
            if (const auto* v = u.if_contains(k); v != nullptr && v->is_int64()) {
                return v->get_int64();
            }
            return 0;
        };
        in_tok = geti("input_tokens");
        out_tok = geti("output_tokens");
        cache_read = geti("cache_read_input_tokens");
        cache_create = geti("cache_creation_input_tokens");
    }

    const std::string summary =
        "lines=" + std::to_string(batch_lines.size()) + " stop_reason=" +
        (answer.stop_reason.empty() ? std::string("(absent)") : answer.stop_reason) +
        " in=" + std::to_string(in_tok) + " out=" + std::to_string(out_tok) +
        " cache_read=" + std::to_string(cache_read) +
        " cache_create=" + std::to_string(cache_create);

    append_raw_answer(summary, answer.text);
    log_info("=== GEMINI RAW ANSWER (" + summary + ") ===");
    log_info(answer.text.substr(0, RAW_ANSWER_PREVIEW));
    if (answer.text.size() > RAW_ANSWER_PREVIEW && !raw_answer_log().empty()) {
        log_info("  [preview cut -- full answer appended to " + raw_answer_log() + "]");
    }
    log_info("=== END RAW ANSWER ===");

    return usable_translations(answer, batch_lines.size());
}

void translate_batch(Cache& cache,
                     std::vector<std::pair<std::string, std::string>>& previous_context,
                     const std::vector<std::pair<std::string, std::string>>& batch,
                     const BatchAnswer& ask, std::chrono::seconds retry_wait) {
    auto apply = [&](const std::map<int, std::string>& translations, bool report_gaps) {
        for (std::size_t i = 0; i < batch.size(); ++i) {
            const auto& [speaker, jp] = batch[i];
            std::string en;
            const auto it = translations.find(static_cast<int>(i) + 1);
            if (it != translations.end()) {
                en = strip(it->second);
            }
            if (en.empty() && report_gaps) {
                log_info("    Missing translation for line " + std::to_string(i + 1) +
                         " -- leaving it queued");
            }
            apply_line_result(cache, previous_context, speaker, jp, en);
        }
    };

    try {
        const auto translations = ask();
        if (translations.size() < batch.size()) {
            log_info("    WARN: got " + std::to_string(translations.size()) + "/" +
                     std::to_string(batch.size()) + " translations");
        }
        apply(translations, true);
    } catch (const gemini::RateLimitError&) {
        log_info("    Rate limited, waiting " + std::to_string(retry_wait.count()) + "s...");
        std::this_thread::sleep_for(retry_wait);
        try {
            apply(ask(), false);
        } catch (const std::exception& e2) {
            log_info(std::string("    Retry failed: ") + e2.what());
            apply({}, false);
        }
    } catch (const std::exception& e) {
        log_info(std::string("    Batch error: ") + e.what());
        apply({}, false);
    }
}

int translate_all(const TranslateOptions& opt) {
    log_info("Model: " + MODEL);
    log_info("Reasoning: " + reasoning_control_label(MODEL));
    log_info("Batch size: " + std::to_string(opt.batch_size) +
             ", context window: " + std::to_string(CONTEXT_WINDOW) +
             ", max_tokens: " + std::to_string(compute_max_tokens(opt.batch_size)));

    log_info("\nLoading extracted text...");
    bj::value all_scripts_v = json_parse_file(opt.input_file);
    bj::object& all_scripts = all_scripts_v.get_object();

    log_info("\n=== Speaker gate (no tokens spent) ===");
    const SpeakerCheckReport gate = run_speaker_checks(all_scripts);
    if (!gate.passed) {
        log_info("\n[ERROR] speaker check " + std::to_string(gate.failed_check) +
                 " failed: " + gate.reason);
        log_info("        Nothing was translated and nothing was written.");
        return kExitGateFailed;
    }

    if (opt.gate_only) {
        log_info("\n--gate-only: the gate passed. No request was sent and no "
                 "file was written.");
        return kExitOk;
    }

    if (opt.fresh_run && gemini::load_api_key().empty()) {
        log_info("[ERROR] GEMINI_API_KEY not set -- refusing to discard the "
                 "cache for a run that cannot translate.");
        return kExitFailure;
    }

    if (opt.fresh_run) {
        for (const std::string& p : {opt.cache_file, opt.output_file}) {
            // Reported, not thrown, at both operations: the throwing overloads
            // name no file, and this run is about to translate from scratch.
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
                return kExitFailure;
            }
            log_info(removed ? "Fresh run: removed " + p
                             : "Fresh run: " + p + " was already gone");
        }
    }

    set_raw_answer_log(raw_answer_log_path(opt.output_file));
    log_info("Raw answers: " + raw_answer_log());

    gemini::LazyClient client;

    log_info("Loading translation cache...");
    Cache cache;
    if (opt.retranslate) {
        log_info("  --retranslate: starting fresh cache");
    } else {
        cache = load_cache(opt.cache_file);
        log_info("  Cache has " + std::to_string(cache.size()) + " entries");
        const std::size_t purged = purge_failed_entries(cache);
        log_info("  Failed entries purged: " + std::to_string(purged) +
                 (purged > 0 ? "  (those lines re-queue)" : ""));
    }

    for (const auto& [jp, en] : name_translations_ordered()) {
        cache.set_default(jp, en);
    }

    std::vector<std::tuple<std::string, std::int64_t, std::int64_t>> scripts_with_text;
    std::int64_t total_uncached = 0;
    {
        std::vector<std::string> names;
        for (const auto& kv : all_scripts) {
            names.emplace_back(kv.key());
        }
        std::sort(names.begin(), names.end());
        for (const auto& script_name : names) {
            if (opt.only_script && script_name != *opt.only_script) {
                continue;
            }
            const auto& script = all_scripts.at(script_name).get_object();
            const auto* strings_v = script.if_contains("strings");
            if (strings_v == nullptr || !strings_v->is_array() ||
                strings_v->get_array().empty()) {
                continue;
            }
            std::int64_t uncached = 0;
            for (const auto& ev : strings_v->get_array()) {
                if (!cache.contains(
                        std::string(ev.get_object().at("text").get_string()))) {
                    ++uncached;
                }
            }
            std::int64_t total = 0;
            if (const auto* j = script.if_contains("jp_strings");
                j != nullptr && j->is_int64()) {
                total = j->get_int64();
            }
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
        log_info("\n=== Translating in script order ===");
        std::int64_t done_lines = 0;
        std::int64_t done_scripts = 0;
        const auto start = std::chrono::steady_clock::now();

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

            const bool stopped = translate_script(
                client, all_scripts.at(script_name).get_object(), cache, opt.batch_size,
                [&](const Cache& c) { save_cache(c, opt.cache_file); },
                opt.test_mode, opt.test_mode ? &batches_remaining : nullptr);
            done_lines += total_in_script;
            ++done_scripts;

            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
                    .count();
            const double rate =
                elapsed > 0.0 ? static_cast<double>(done_lines) / elapsed : 0.0;
            char buf[kStatsBufChars];
            std::snprintf(buf, sizeof(buf), "    %.1f lines/s  done=%lld", rate,
                          static_cast<long long>(done_lines));
            log_info(buf);

            if (stopped) {
                log_info("\n  --test mode: stopping after requested batches");
                break;
            }
        }

        save_cache(cache, opt.cache_file);
        log_info("\nTranslation pass done. Cache: " + std::to_string(cache.size()) +
                 " entries");
    }

    log_info("\nWriting translated_text.json...");
    for (auto& kv : all_scripts) {
        auto& script = kv.value().get_object();
        auto* strings_v = script.if_contains("strings");
        if (strings_v == nullptr || !strings_v->is_array()) {
            continue;
        }
        for (auto& ev : strings_v->get_array()) {
            auto& entry = ev.get_object();
            std::string jp(entry.at("text").get_string());
            if (const std::string* en = cache.get(jp)) {
                entry["translated"] = *en;
            } else {
                entry["translated"] = jp;
            }
        }
    }

    write_file(opt.output_file, json_dump(all_scripts_v));
    char sz[kStatsBufChars];
    std::snprintf(
        sz, sizeof(sz), "%.1f MB",
        static_cast<double>(fs::file_size(fs::u8path(opt.output_file))) / kBytesPerMegabyte);
    log_info("  -> " + opt.output_file + " (" + sz + ")");

    print_line("");
    report_coverage(all_scripts, opt.output_file);

    print_line("");
    log_info("Building the runtime translation table...");
    return build_tsv::run_build(opt.output_file, opt.cache_file, opt.tsv_file,
                                opt.include_untranslated, opt.flow_breaks);
}

}  // namespace snt::translate
