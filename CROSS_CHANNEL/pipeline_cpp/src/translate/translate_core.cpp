// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "translate/translate_core.h"

#include "build_tsv/build_tsv.h"
#include "common/util.h"
#include "translate/speaker_gate.h"

#include <boost/json.hpp>
#include <boost/regex.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace crc::translate {
namespace {

namespace bj = boost::json;
namespace fs = std::filesystem;

constexpr std::int64_t kTokensPerBatchLine = 300;
constexpr std::size_t kCrlfBytes = 2;
constexpr int kDecimalBase = 10;
constexpr std::size_t kMaxMarkdownStars = 2;
constexpr unsigned char kAsciiLimit = 0x80U;

constexpr char32_t kHiraganaFirst = 0x3040;
constexpr char32_t kHiraganaLast = 0x309F;
constexpr char32_t kKatakanaFirst = 0x30A0;
constexpr char32_t kKatakanaLast = 0x30FF;
constexpr char32_t kCjkExtAFirst = 0x3400;
constexpr char32_t kCjkExtALast = 0x4DBF;
constexpr char32_t kCjkUnifiedFirst = 0x4E00;
constexpr char32_t kCjkUnifiedLast = 0x9FFF;
constexpr char32_t kHalfwidthKatakanaFirst = 0xFF66;
constexpr char32_t kHalfwidthKatakanaLast = 0xFF9F;
constexpr char32_t kFullwidthZero = 0xFF10;
constexpr char32_t kFullwidthNine = 0xFF19;
constexpr char32_t kFullwidthRightParen = 0xFF09;
constexpr char32_t kFullwidthFullStop = 0xFF0E;
constexpr char32_t kFullwidthColon = 0xFF1A;

constexpr double kTokensPerMillion = 1'000'000.0;
constexpr double kInputCostPerMillionUsd = 2.00;
constexpr double kCachedInputCostPerMillionUsd = 0.20;
constexpr double kArgonOutputCostPerMillionUsd = 10.00;
constexpr double kGemini31ProOutputCostPerMillionUsd = 12.00;
constexpr double kUsdToEurConservativeFactor = 0.95;

constexpr int kCacheJsonIndent = 1;
constexpr int kTranslatedJsonIndent = 1;
constexpr int kAuxiliaryJsonIndent = 2;
constexpr std::size_t kContextRetentionMultiplier = 2;
constexpr std::size_t kMinRetainedContextLines = 120;
constexpr std::size_t kBinarySplitDivisor = 2;
constexpr int kMaxIncompleteRetries = 2;
constexpr std::int64_t kMaxCallsPerBoundedBatch = 256;
constexpr std::size_t kTelemetryBufferBytes = 256;
constexpr std::size_t kCostSummaryBufferBytes = 128;
constexpr std::size_t kMaxSpeakerCodepoints = 20;

constexpr std::string_view kMaxTokensFinishReason = "MAX_TOKENS";
constexpr std::string_view kGemini31ProPrefix = "gemini-3.1-pro";
constexpr std::string_view kNarrationTag = "NARRATION";
constexpr std::string_view kEngineLineBreakToken = "%N";
constexpr std::string_view kMarkdownCodeFence = "```";
constexpr std::string_view kDefaultBlockReason = "CONTENT_BLOCKED";

struct StreamEntry {
    std::int64_t offset = 0;
    std::string speaker;
    std::string jp;
};

std::string replace_all_substrings(std::string input,
                                   std::string_view from,
                                   std::string_view to) {
    if (from.empty()) {
        return input;
    }
    std::string out;
    out.reserve(input.size());
    std::size_t pos = 0;
    for (;;) {
        const std::size_t hit = input.find(from, pos);
        if (hit == std::string::npos) {
            out.append(input, pos, std::string::npos);
            return out;
        }
        out.append(input, pos, hit - pos);
        out.append(to.data(), to.size());
        pos = hit + from.size();
    }
}

std::size_t count_non_overlapping(std::string_view haystack,
                                  std::string_view needle) noexcept {
    if (needle.empty() || needle.size() > haystack.size()) {
        return 0;
    }
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string_view::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

std::size_t codepoint_length(const std::string& utf8) {
    std::size_t count = 0;
    std::size_t idx = 0;
    while (idx < utf8.size()) {
        utf8_next(utf8, idx);
        ++count;
    }
    return count;
}

bool has_non_ascii_bytes(std::string_view s) noexcept {
    for (const char ch : s) {
        if (static_cast<unsigned char>(ch) >= kAsciiLimit) {
            return true;
        }
    }
    return false;
}

std::string trim_horizontal_spaces(std::string_view s) {
    std::size_t first = 0;
    while (first < s.size() && (s[first] == ' ' || s[first] == '\t')) {
        ++first;
    }
    std::size_t last = s.size();
    while (last > first && (s[last - 1U] == ' ' || s[last - 1U] == '\t')) {
        --last;
    }
    return std::string(s.substr(first, last - first));
}

std::string to_lower_ascii(std::string_view s) {
    std::string out(s);
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return out;
}

bool starts_with_bracket_in_jp(const std::string& jp) {
    const std::string trimmed_jp = trim(jp);
    if (trimmed_jp.empty()) {
        return false;
    }
    std::size_t idx = 0;
    const char32_t first_cp = utf8_next(trimmed_jp, idx);
    return first_cp == U'[' || first_cp == U'［' || first_cp == U'【';
}

bool is_known_speaker_label(std::string_view label) {
    const std::string trimmed_label = trim(std::string(label));
    const std::string lowered = to_lower_ascii(trimmed_label);
    if (lowered.empty()) {
        return false;
    }
    if (lowered == "narration" || lowered == "speaker") {
        return true;
    }
    for (const auto& [jp_name, en_name] : name_translations_ordered()) {
        if (trimmed_label == jp_name || lowered == to_lower_ascii(en_name)) {
            return true;
        }
    }
    return false;
}

std::string strip_known_speaker_tags(const std::string& raw) {
    static const boost::regex kLeadingTagRe(
        R"re(^\[([^|\r\n\]]{1,32})\]\s*(?=\S))re");

    std::string cur = raw;
    for (;;) {
        boost::smatch match;
        if (!boost::regex_search(cur, match, kLeadingTagRe)) {
            break;
        }
        if (!is_known_speaker_label(match[1].str())) {
            break;
        }
        cur = cur.substr(static_cast<std::size_t>(match[0].length()));
    }
    return cur;
}

std::string strip_accidental_speaker_tags(const std::string& jp,
                                          const std::string& raw) {
    static const boost::regex kLeadingTagRe(
        R"re(^\[([^|\r\n\]]{1,32})\]\s*(?=\S))re");
    static const boost::regex kAsciiSpeakerCharsRe(
        R"re(^(?:NARRATION|[A-Za-z0-9\s()'"?&$.,!_/*+-]+)$)re");

    const bool jp_has_leading_bracket = starts_with_bracket_in_jp(jp);
    std::string cur = raw;
    for (;;) {
        boost::smatch match;
        if (!boost::regex_search(cur, match, kLeadingTagRe)) {
            break;
        }
        const std::string tag_inner = match[1].str();
        const bool known_speaker = is_known_speaker_label(tag_inner);
        const bool looks_like_speaker_tag =
            known_speaker ||
            (!jp_has_leading_bracket &&
             boost::regex_match(tag_inner, kAsciiSpeakerCharsRe));
        if (!looks_like_speaker_tag) {
            break;
        }
        cur = cur.substr(static_cast<std::size_t>(match[0].length()));
    }
    return cur;
}

std::string unescape_prompt_newlines(std::string s) {
    s = replace_all_substrings(std::move(s), "\\r\\n", "\r\n");
    s = replace_all_substrings(std::move(s), "\\n", "\n");
    return s;
}

std::string replace_extra_non_ascii_punctuation(std::string s) {
    s = replace_all_substrings(std::move(s), "―", "--");
    s = replace_all_substrings(std::move(s), "─", "--");
    s = replace_all_substrings(std::move(s), "⋯", "...");
    s = replace_all_substrings(std::move(s), "「", "\"");
    s = replace_all_substrings(std::move(s), "」", "\"");
    s = replace_all_substrings(std::move(s), "『", "'");
    s = replace_all_substrings(std::move(s), "』", "'");
    s = replace_all_substrings(std::move(s), "\u301C", "~");
    s = replace_all_substrings(std::move(s), "\uFF5E", "~");
    s = replace_all_substrings(std::move(s), "é", "e");
    s = replace_all_substrings(std::move(s), "ê", "e");
    s = replace_all_substrings(std::move(s), "è", "e");
    s = replace_all_substrings(std::move(s), "ë", "e");
    s = replace_all_substrings(std::move(s), "É", "E");
    s = replace_all_substrings(std::move(s), "Ê", "E");
    s = replace_all_substrings(std::move(s), "È", "E");

    static const boost::regex kRubyBracketRe(R"re(\[([^|\]]+)\|([^\]]+)\])re");
    s = boost::regex_replace(s, kRubyBracketRe, "$2");
    return s;
}

std::string clean_translated_line_impl(const std::string& jp,
                                       const std::string& raw_en,
                                       bool unescape_newlines) {
    std::string s = trim(raw_en);
    if (s.empty() || s == jp) {
        return s;
    }

    s = strip_accidental_speaker_tags(jp, s);
    s = trim(s);
    if (s.empty() || s == jp) {
        return s;
    }

    if (unescape_newlines) {
        s = unescape_prompt_newlines(std::move(s));
    }
    s = replace_extra_non_ascii_punctuation(std::move(s));
    s = trim_horizontal_spaces(s);
    if (s.empty() || s == jp) {
        return s;
    }

    return postprocess(jp, s);
}

std::string resolve_prompt_speaker(const std::string& speaker) {
    const std::string trimmed = trim(speaker);
    if (trimmed.empty()) {
        return std::string(kNarrationTag);
    }
    const auto& glossary = name_translations();
    const auto it = glossary.find(trimmed);
    return it != glossary.end() ? it->second : trimmed;
}

int digit_value(char32_t cp) noexcept {
    if (cp >= U'0' && cp <= U'9') {
        return static_cast<int>(cp - U'0');
    }
    if (cp >= kFullwidthZero && cp <= kFullwidthNine) {
        return static_cast<int>(cp - kFullwidthZero);
    }
    return -1;
}

bool is_opener_delimiter(char32_t cp) noexcept {
    return cp == U'.' || cp == U')' || cp == U':' ||
           cp == kFullwidthFullStop || cp == kFullwidthRightParen ||
           cp == kFullwidthColon;
}

std::size_t consume_markdown_stars(const std::string& s, std::size_t pos) noexcept {
    std::size_t consumed = 0;
    while (pos + consumed < s.size() &&
           s[pos + consumed] == '*' &&
           consumed < kMaxMarkdownStars) {
        ++consumed;
    }
    return pos + consumed;
}

enum class LineParseKind {
    ValidEntry,
    IncompleteOpenerOnly,
    MalformedOpener,
    ContinuationOrOther,
};

struct ParsedLineOpener {
    LineParseKind kind = LineParseKind::ContinuationOrOther;
    int number = 0;
    std::string body;
};

ParsedLineOpener parse_single_numbered_line(const std::string& trimmed_line) {
    ParsedLineOpener out;
    std::size_t idx = 0;
    const std::size_t lead_stars_end = consume_markdown_stars(trimmed_line, idx);
    const std::size_t lead_stars = lead_stars_end - idx;
    idx = lead_stars_end;

    bool negative_sign = false;
    if (idx < trimmed_line.size() && trimmed_line[idx] == '-') {
        negative_sign = true;
        ++idx;
    }

    int parsed_num = 0;
    bool has_digit = false;
    bool overflowed = false;
    while (idx < trimmed_line.size()) {
        const std::size_t before_cp = idx;
        const char32_t cp = utf8_next(trimmed_line, idx);
        const int dv = digit_value(cp);
        if (dv < 0) {
            idx = before_cp;
            break;
        }
        has_digit = true;
        if (parsed_num > (std::numeric_limits<int>::max() - dv) / kDecimalBase) {
            overflowed = true;
        } else {
            parsed_num = parsed_num * kDecimalBase + dv;
        }
    }

    if (!has_digit) {
        return out;
    }

    std::size_t stars_used = 0;
    if (lead_stars > 0) {
        const std::size_t after_pre_delim_stars =
            consume_markdown_stars(trimmed_line, idx);
        stars_used += (after_pre_delim_stars - idx);
        idx = after_pre_delim_stars;
    }

    if (idx >= trimmed_line.size()) {
        if (negative_sign || overflowed) {
            out.kind = LineParseKind::MalformedOpener;
            return out;
        }
        out.kind = LineParseKind::IncompleteOpenerOnly;
        out.number = parsed_num;
        return out;
    }

    const char32_t delim_cp = utf8_next(trimmed_line, idx);
    if (!is_opener_delimiter(delim_cp)) {
        return out;
    }

    if ((delim_cp == U'.' || delim_cp == U':') && idx < trimmed_line.size()) {
        const char next_ch = trimmed_line[idx];
        if (next_ch >= '0' && next_ch <= '9') {
            return out;
        }
    }

    if (negative_sign || overflowed) {
        out.kind = LineParseKind::MalformedOpener;
        return out;
    }

    if (lead_stars > stars_used) {
        const std::size_t rem_limit = lead_stars - stars_used;
        std::size_t extra = 0;
        while (idx + extra < trimmed_line.size() &&
               trimmed_line[idx + extra] == '*' &&
               extra < rem_limit) {
            ++extra;
        }
        idx += extra;
    }

    std::string rest = trim(trimmed_line.substr(idx));
    if (rest.empty()) {
        out.kind = LineParseKind::IncompleteOpenerOnly;
        out.number = parsed_num;
        return out;
    }

    out.kind = LineParseKind::ValidEntry;
    out.number = parsed_num;
    out.body = std::move(rest);
    return out;
}

std::string unwrap_outer_markdown_fence(const std::string& text) {
    const std::string trimmed_all = trim(text);
    if (trimmed_all.size() < kMarkdownCodeFence.size() * 2U ||
        trimmed_all.compare(0, kMarkdownCodeFence.size(), kMarkdownCodeFence) != 0 ||
        trimmed_all.compare(trimmed_all.size() - kMarkdownCodeFence.size(),
                            kMarkdownCodeFence.size(),
                            kMarkdownCodeFence) != 0) {
        return text;
    }
    const std::size_t first_nl = trimmed_all.find('\n');
    if (first_nl == std::string::npos ||
        first_nl >= trimmed_all.size() - kMarkdownCodeFence.size()) {
        return text;
    }
    return trimmed_all.substr(
        first_nl + 1U,
        trimmed_all.size() - first_nl - 1U - kMarkdownCodeFence.size());
}

std::string normalize_parsed_body(std::string body) {
    body = strip_known_speaker_tags(body);
    body = unescape_prompt_newlines(std::move(body));
    return trim_horizontal_spaces(body);
}

bool is_japanese_kana_or_kanji(char32_t cp) noexcept {
    return (cp >= kHiraganaFirst && cp <= kHiraganaLast) ||
           (cp >= kKatakanaFirst && cp <= kKatakanaLast) ||
           (cp >= kHalfwidthKatakanaFirst && cp <= kHalfwidthKatakanaLast) ||
           (cp >= kCjkExtAFirst && cp <= kCjkExtALast) ||
           (cp >= kCjkUnifiedFirst && cp <= kCjkUnifiedLast);
}

bool contains_leaked_kana_or_kanji(const std::string& utf8) {
    std::size_t idx = 0;
    while (idx < utf8.size()) {
        if (is_japanese_kana_or_kanji(utf8_next(utf8, idx))) {
            return true;
        }
    }
    return false;
}

std::optional<std::pair<std::string, std::string>> find_legacy_speaker_plate(
    const std::string& original) {
    const std::size_t crlf = original.find("\r\n");
    if (crlf == std::string::npos) {
        return std::nullopt;
    }
    const std::string jp_name = trim(original.substr(0, crlf));
    if (jp_name.empty() || codepoint_length(jp_name) > kMaxSpeakerCodepoints) {
        return std::nullopt;
    }
    const auto& glossary = name_translations();
    const auto it = glossary.find(jp_name);
    if (it == glossary.end()) {
        return std::nullopt;
    }
    return std::make_pair(jp_name, it->second);
}

std::string strip_speaker_plate_body(const std::string& jp,
                                     const std::string& text) {
    const auto plate = find_legacy_speaker_plate(jp);
    if (!plate.has_value()) {
        return text;
    }
    const std::string en_prefix = plate->second + "\r\n";
    if (text.rfind(en_prefix, 0) == 0) {
        return text.substr(en_prefix.size());
    }
    const std::string jp_prefix = plate->first + "\r\n";
    if (text.rfind(jp_prefix, 0) == 0) {
        return text.substr(jp_prefix.size());
    }
    return text;
}

bool ends_with_crlf(std::string_view s) noexcept {
    return s.size() >= kCrlfBytes &&
           s.compare(s.size() - kCrlfBytes, kCrlfBytes, "\r\n") == 0;
}

bool ends_with_bare_lf(std::string_view s) noexcept {
    return !s.empty() && s.back() == '\n' && !ends_with_crlf(s);
}

bool is_gemini_31_pro_model(std::string_view model) noexcept {
    return model.find(kGemini31ProPrefix) != std::string_view::npos;
}

std::string json_str(const bj::object& obj, const char* key) {
    if (const auto* v = obj.if_contains(key)) {
        if (v->is_string()) {
            return std::string(v->get_string());
        }
    }
    return {};
}

std::int64_t json_int64(const bj::object& obj, const char* key) noexcept {
    if (const auto* v = obj.if_contains(key)) {
        if (v->is_int64()) {
            return v->get_int64();
        }
        if (v->is_uint64()) {
            const std::uint64_t u = v->get_uint64();
            constexpr auto kMaxI64 =
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
            return static_cast<std::int64_t>(u > kMaxI64 ? kMaxI64 : u);
        }
    }
    return 0;
}

void ensure_parent_dir(const std::string& file_path) {
    if (file_path.empty()) {
        return;
    }
    const fs::path parent = fs::u8path(file_path).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        fs::create_directories(parent, ec);
    }
}

std::vector<BlockedLine> load_existing_blocked_lines(
    const std::string& blocked_file) {
    std::vector<BlockedLine> out;
    if (blocked_file.empty()) {
        return out;
    }
    std::error_code ec;
    if (!fs::exists(fs::u8path(blocked_file), ec) || ec) {
        return out;
    }
    try {
        const bj::value parsed = json_parse_file(blocked_file);
        if (!parsed.is_array()) {
            return out;
        }
        for (const auto& item : parsed.get_array()) {
            if (!item.is_object()) {
                continue;
            }
            const auto& obj = item.get_object();
            BlockedLine b;
            b.offset = json_int64(obj, "offset");
            b.speaker = json_str(obj, "speaker");
            b.jp = json_str(obj, "jp");
            b.reason = json_str(obj, "reason");
            if (!b.jp.empty()) {
                out.push_back(std::move(b));
            }
        }
    } catch (const std::exception&) {
        return {};
    }
    return out;
}

void save_blocked_lines_atomic(const std::string& blocked_file,
                               const std::vector<BlockedLine>& blocked) {
    if (blocked_file.empty()) {
        return;
    }
    ensure_parent_dir(blocked_file);
    bj::array arr;
    arr.reserve(blocked.size());
    for (const BlockedLine& b : blocked) {
        bj::object obj;
        obj["offset"] = b.offset;
        obj["speaker"] = b.speaker;
        obj["jp"] = b.jp;
        obj["reason"] = b.reason;
        arr.push_back(std::move(obj));
    }
    write_file_atomic_text(
        blocked_file, json_pretty(bj::value(std::move(arr)), kAuxiliaryJsonIndent));
}

void append_raw_answer_log(const std::string& raw_log_file,
                           int batch_no,
                           std::size_t slice_size,
                           bool neutral_prompt,
                           const GeminiResponse& resp) {
    if (raw_log_file.empty()) {
        return;
    }
    ensure_parent_dir(raw_log_file);
    std::ofstream out(fs::u8path(raw_log_file), std::ios::binary | std::ios::app);
    if (!out) {
        return;
    }
    out << "=== batch=" << batch_no
        << " lines=" << slice_size
        << " prompt=" << (neutral_prompt ? "neutral" : "primary")
        << " finish=" << resp.finish_reason
        << " blocked=" << (resp.content_blocked ? 1 : 0)
        << " in=" << resp.prompt_tokens
        << " cached=" << resp.cached_tokens
        << " out=" << resp.output_tokens
        << " thought=" << resp.thought_tokens << " ===\r\n"
        << resp.text << "\r\n\r\n";
}

void write_last_batch_snapshot(const std::string& last_batch_file,
                               int batch_no,
                               int total_batches,
                               std::size_t cache_size,
                               const std::vector<PromptLine>& batch_lines,
                               const Cache& cache) {
    if (last_batch_file.empty()) {
        return;
    }
    ensure_parent_dir(last_batch_file);
    bj::array entries_arr;
    entries_arr.reserve(batch_lines.size());
    for (const PromptLine& line : batch_lines) {
        bj::object item;
        item["offset"] = line.offset;
        item["speaker"] = line.speaker;
        item["jp"] = line.jp;
        const std::string* en = cache.get(line.jp);
        item["en"] = en != nullptr ? *en : std::string{};
        entries_arr.push_back(std::move(item));
    }
    bj::object root;
    root["batch"] = static_cast<std::int64_t>(batch_no);
    root["total_batches"] = static_cast<std::int64_t>(total_batches);
    root["cache_size"] = static_cast<std::int64_t>(cache_size);
    root["entries"] = std::move(entries_arr);
    write_file_atomic_text(
        last_batch_file,
        json_pretty(bj::value(std::move(root)), kAuxiliaryJsonIndent));
}

bool remove_file_if_present(const std::string& path_str) {
    if (path_str.empty()) {
        return true;
    }
    const fs::path path = fs::u8path(path_str);
    std::error_code ec;
    const bool present = fs::exists(path, ec);
    if (!ec && !present) {
        return true;
    }
    fs::remove(path, ec);
    if (ec) {
        log_info("[ERROR] cannot delete " + path_str + ": " + ec.message());
        return false;
    }
    return true;
}

std::vector<std::string> build_ordered_file_list(const bj::object& all_data) {
    std::vector<std::string> file_order;
    std::set<std::string> seen;
    for (const std::string& f : story_order()) {
        if (all_data.if_contains(f) != nullptr && seen.insert(f).second) {
            file_order.push_back(f);
        }
    }
    for (const auto& kv : all_data) {
        const std::string name(kv.key());
        if (seen.insert(name).second) {
            file_order.push_back(name);
        }
    }
    return file_order;
}

std::size_t count_total_extracted_entries(const bj::object& all_data) {
    std::size_t total = 0;
    for (const auto& kv : all_data) {
        if (!kv.value().is_object()) {
            continue;
        }
        const auto* strings_val = kv.value().get_object().if_contains("strings");
        if (strings_val != nullptr && strings_val->is_array()) {
            total += strings_val->get_array().size();
        }
    }
    return total;
}

std::vector<StreamEntry> collect_file_translatable_entries(
    const bj::object& fdata) {
    std::vector<StreamEntry> entries;
    const std::vector<std::size_t> idxs = translatable_indices(fdata);
    if (idxs.empty()) {
        return entries;
    }
    const auto* strings_val = fdata.if_contains("strings");
    if (strings_val == nullptr || !strings_val->is_array()) {
        return entries;
    }
    const auto& arr = strings_val->get_array();
    entries.reserve(idxs.size());
    for (const std::size_t idx : idxs) {
        const auto& s = arr[idx].get_object();
        StreamEntry entry;
        entry.offset = json_int64(s, "offset");
        entry.speaker = extract_speaker(s);
        entry.jp = json_str(s, "text");
        entries.push_back(std::move(entry));
    }
    return entries;
}

void write_translated_document_atomic(bj::value& all_data_v,
                                      const Cache& cache,
                                      const std::string& output_file) {
    bj::object& all_data = all_data_v.get_object();
    for (auto& kv : all_data) {
        if (!kv.value().is_object()) {
            continue;
        }
        auto* strings = kv.value().get_object().if_contains("strings");
        if (strings == nullptr || !strings->is_array()) {
            continue;
        }
        for (auto& sv : strings->get_array()) {
            if (!sv.is_object()) {
                continue;
            }
            auto& s = sv.get_object();
            const std::string text = json_str(s, "text");
            const std::string* en = cache.get(text);
            s["translated"] =
                (en != nullptr && *en != text) ? postprocess(text, *en) : text;
        }
    }
    ensure_parent_dir(output_file);
    write_file_atomic_text(
        output_file, json_pretty(all_data_v, kTranslatedJsonIndent));
    log_info("Saved " + output_file);
}

void seed_glossary_entries(Cache& cache) {
    for (const auto& [jp, en] : name_translations_ordered()) {
        cache.set_default(jp, en);
    }
}

void log_final_translation_summary(const std::string& model,
                                   const TranslateReport& report) {
    char cost_buf[kCostSummaryBufferBytes];
    std::snprintf(cost_buf, sizeof(cost_buf),
                  "$%.4f (EUR %.4f)",
                  report.estimated_usd,
                  report.estimated_eur);

    log_info("\nTranslation summary:");
    log_info("  Model:              " + model);
    log_info("  API calls:          " + comma(report.usage.api_calls));
    log_info("  New translations:   " +
             comma(static_cast<long long>(report.translated_new)));
    log_info("  Cached total:       " +
             comma(static_cast<long long>(report.cached_total)));
    log_info("  Remaining uncached: " +
             comma(static_cast<long long>(report.remaining_uncached)));
    log_info("  Blocked lines:      " +
             comma(static_cast<long long>(report.blocked_count)));
    log_info("  Tokens:             prompt=" + comma(report.usage.prompt_tokens) +
             " cached=" + comma(report.usage.cached_tokens) +
             " output=" + comma(report.usage.output_tokens) +
             " thought=" + comma(report.usage.thought_tokens));
    log_info(std::string("  Estimated cost:     ") + cost_buf);
}

}  // namespace

bool has_real_japanese(const std::string& text) {
    return is_jp(text);
}

bool needs_translation(const std::string& text) {
    const std::string trimmed = trim(text);
    return !trimmed.empty() && has_real_japanese(trimmed);
}

std::vector<std::size_t> translatable_indices(const bj::object& fdata) {
    std::vector<std::size_t> indices;
    const auto* strings = fdata.if_contains("strings");
    if (strings == nullptr || !strings->is_array()) {
        return indices;
    }
    const auto& arr = strings->get_array();
    indices.reserve(arr.size());
    for (std::size_t i = 0; i < arr.size(); ++i) {
        if (!arr[i].is_object()) {
            continue;
        }
        const std::string text = json_str(arr[i].get_object(), "text");
        if (needs_translation(text)) {
            indices.push_back(i);
        }
    }
    return indices;
}

std::string extract_speaker(const bj::object& s) {
    std::string raw = trim(json_str(s, "speaker"));
    if (raw.empty() && json_str(s, "type") != "narration") {
        const std::string text = json_str(s, "text");
        const std::size_t crlf = text.find("\r\n");
        if (crlf != std::string::npos) {
            raw = trim(text.substr(0, crlf));
        }
    }
    if (raw.empty() || codepoint_length(raw) > kMaxSpeakerCodepoints) {
        return std::string(kNarrationTag);
    }
    const auto& glossary = name_translations();
    const auto it = glossary.find(raw);
    return it != glossary.end() ? it->second : raw;
}

std::string sanitize_ascii(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    std::size_t idx = 0;
    while (idx < text.size()) {
        const std::size_t start = idx;
        const char32_t cp = utf8_next(text, idx);
        switch (cp) {
        case U'\u2018':
        case U'\u2019':
        case U'\u201A':
        case U'\u201B':
        case U'\uFF07':
            out.push_back('\'');
            break;
        case U'\u201C':
        case U'\u201D':
        case U'\u201E':
        case U'\u201F':
        case U'\uFF02':
            out.push_back('"');
            break;
        case U'\u2013':
        case U'\u2014':
        case U'\u2015':
        case U'\u2500':
            out += "--";
            break;
        case U'\u2012':
        case U'\u2212':
            out.push_back('-');
            break;
        case U'\u2026':
        case U'\u22EF':
            out += "...";
            break;
        case U'\u00A0':
        case U'\u2000':
        case U'\u2001':
        case U'\u2002':
        case U'\u2003':
        case U'\u2004':
        case U'\u2005':
        case U'\u2006':
        case U'\u2007':
        case U'\u2008':
        case U'\u2009':
        case U'\u200A':
        case U'\u202F':
        case U'\u205F':
        case U'\u3000':
            out.push_back(' ');
            break;
        case U'\u301C':
        case U'\uFF5E':
            out.push_back('~');
            break;
        case U'\u200B':
        case U'\uFEFF':
            break;
        default:
            out.append(text, start, idx - start);
            break;
        }
    }
    return out;
}

std::string postprocess(const std::string& original, const std::string& translated) {
    std::string res = trim(sanitize_ascii(translated));
    if (res.empty()) {
        return res;
    }

    if (const auto plate = find_legacy_speaker_plate(original)) {
        const std::string en_prefix = plate->second + "\r\n";
        const std::string jp_prefix = plate->first + "\r\n";
        if (res.rfind(en_prefix, 0) == 0) {
            res = en_prefix + trim(res.substr(en_prefix.size()));
        } else if (res.rfind(jp_prefix, 0) == 0) {
            res = en_prefix + trim(res.substr(jp_prefix.size()));
        } else {
            res = en_prefix + res;
        }
    } else if (original.find("\r\n") == std::string::npos) {
        const std::size_t crlf = res.find("\r\n");
        if (crlf != std::string::npos) {
            const std::string prefix = trim_horizontal_spaces(
                std::string_view(res.data(), crlf));
            if (is_known_speaker_label(prefix)) {
                res = trim(res.substr(crlf + kCrlfBytes));
            }
        }
    }

    res = trim_right(res);
    if (res.empty()) {
        return res;
    }

    if (ends_with_crlf(original)) {
        res += "\r\n";
        return res;
    }
    if (ends_with_bare_lf(original)) {
        res.push_back('\n');
        return res;
    }
    return res;
}

std::string clean_translated_line(const std::string& jp,
                                  const std::string& raw_en) {
    return clean_translated_line_impl(jp, raw_en, /*unescape_newlines=*/true);
}

Cache load_cache(const std::string& cache_file) {
    Cache cache;
    if (cache_file.empty()) {
        return cache;
    }
    std::error_code ec;
    if (!fs::exists(fs::u8path(cache_file), ec) || ec) {
        return cache;
    }
    const bj::value v = json_parse_file(cache_file);
    if (!v.is_object()) {
        throw std::runtime_error("cache root must be a JSON object: " + cache_file);
    }
    for (const auto& kv : v.get_object()) {
        if (kv.value().is_string()) {
            cache.set(std::string(kv.key()), std::string(kv.value().get_string()));
        }
    }
    return cache;
}

void save_cache(const Cache& cache, const std::string& cache_file) {
    if (cache_file.empty()) {
        return;
    }
    ensure_parent_dir(cache_file);
    bj::object obj;
    obj.reserve(cache.size());
    for (const auto& [jp, en] : cache.items()) {
        obj[jp] = en;
    }
    write_file_atomic_text(
        cache_file, json_pretty(bj::value(std::move(obj)), kCacheJsonIndent));
}

std::size_t cache_entry_count(const std::string& cache_file) {
    if (cache_file.empty()) {
        return 0;
    }
    std::error_code ec;
    if (!fs::exists(fs::u8path(cache_file), ec) || ec) {
        return 0;
    }
    const bj::value v = json_parse_file(cache_file);
    if (!v.is_object()) {
        throw std::runtime_error("cache root must be a JSON object: " + cache_file);
    }
    return v.get_object().size();
}

std::optional<std::string> refuse_cache_discard(std::size_t cache_entries,
                                                const std::string& flag,
                                                bool discard_cache) {
    if (discard_cache || cache_entries <= CACHE_DISCARD_THRESHOLD) {
        return std::nullopt;
    }
    return "refusing " + flag + ": existing cache has " +
           comma(static_cast<long long>(cache_entries)) +
           " entries (>" + std::to_string(CACHE_DISCARD_THRESHOLD) +
           "); pass --discard-cache to confirm deleting paid translations";
}

bool is_failed_entry(const std::string& jp, const std::string& en) {
    const std::string trimmed_jp = trim(jp);
    const std::string trimmed_en = trim(en);
    if (!trimmed_jp.empty() && trimmed_en.empty()) {
        return true;
    }
    if (has_non_ascii_bytes(jp) && (en == jp || trimmed_en == trimmed_jp)) {
        return true;
    }
    return contains_leaked_kana_or_kanji(en);
}

std::size_t purge_failed_entries(Cache& cache) {
    Cache cleaned;
    std::size_t purged = 0;
    for (const auto& [jp, en] : cache.items()) {
        if (is_failed_entry(jp, en)) {
            ++purged;
            continue;
        }
        cleaned.set(jp, en);
    }
    if (purged > 0) {
        cache = std::move(cleaned);
    }
    return purged;
}

std::map<int, std::string> parse_numbered_response(
    const std::string& response_text) {
    std::map<int, std::string> entries;
    if (response_text.empty()) {
        return entries;
    }
    const std::string unwrapped = unwrap_outer_markdown_fence(response_text);
    int current_number = 0;
    std::size_t pos = 0;
    while (pos < unwrapped.size()) {
        const std::size_t nl = unwrapped.find('\n', pos);
        const bool has_nl = (nl != std::string::npos);
        const std::string raw_slice =
            has_nl ? unwrapped.substr(pos, nl - pos) : unwrapped.substr(pos);
        pos = has_nl ? (nl + 1U) : unwrapped.size();

        const std::string trimmed_line =
            trim(strip_known_speaker_tags(trim(raw_slice)));
        if (trimmed_line.empty()) {
            continue;
        }
        const ParsedLineOpener parsed = parse_single_numbered_line(trimmed_line);
        if (parsed.kind == LineParseKind::ValidEntry && parsed.number >= 1) {
            current_number = parsed.number;
            entries[current_number] = normalize_parsed_body(parsed.body);
            continue;
        }
        if (parsed.kind == LineParseKind::ContinuationOrOther &&
            current_number >= 1) {
            std::string& prev = entries[current_number];
            const std::string cont = normalize_parsed_body(trimmed_line);
            if (!cont.empty()) {
                prev += " " + cont;
            }
        }
    }
    return entries;
}

std::int64_t compute_max_output_tokens(int batch_size) {
    const std::int64_t scaled =
        static_cast<std::int64_t>(std::max(0, batch_size)) * kTokensPerBatchLine;
    return std::clamp<std::int64_t>(
        scaled, kDefaultMaxOutputTokens, kGeminiMaxOutputTokens);
}

std::string escape_prompt_line(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text.compare(i, kCrlfBytes, "\r\n") == 0) {
            out += "\\r\\n";
            ++i;
            continue;
        }
        if (text[i] == '\n') {
            out += "\\n";
            continue;
        }
        if (text[i] == '\r') {
            out += "\\r";
            continue;
        }
        out.push_back(text[i]);
    }
    return out;
}

std::string build_batch_user_prompt(
    const std::vector<PromptLine>& batch_lines,
    const std::vector<std::pair<std::string, std::string>>& previous_context,
    const std::vector<PromptLine>& context_after,
    int context_window,
    int context_after_window) {
    std::string prompt;

    const bool has_prev_ctx = (context_window > 0 && !previous_context.empty());
    const bool has_after_ctx =
        (context_after_window > 0 && !context_after.empty());

    if (has_prev_ctx) {
        const auto max_prev = static_cast<std::size_t>(context_window);
        const std::size_t start =
            previous_context.size() > max_prev
                ? (previous_context.size() - max_prev)
                : 0U;
        prompt += "<previous_context>\n";
        for (std::size_t i = start; i < previous_context.size(); ++i) {
            const std::string sp =
                resolve_prompt_speaker(previous_context[i].first);
            prompt += "[" + sp + "] " +
                      escape_prompt_line(previous_context[i].second) + "\n";
        }
        prompt += "</previous_context>\n\n";
    }

    prompt += "<lines_to_translate>\n";
    for (std::size_t i = 0; i < batch_lines.size(); ++i) {
        const std::string sp = resolve_prompt_speaker(batch_lines[i].speaker);
        prompt += std::to_string(i + 1U) + ". [" + sp + "] " +
                  escape_prompt_line(batch_lines[i].jp) + "\n";
    }
    prompt += "</lines_to_translate>\n\n";

    if (has_after_ctx) {
        const std::size_t take = std::min(
            context_after.size(),
            static_cast<std::size_t>(context_after_window));
        prompt += "<context_after>\n";
        for (std::size_t i = 0; i < take; ++i) {
            const std::string sp = resolve_prompt_speaker(context_after[i].speaker);
            prompt += "[" + sp + "] " +
                      escape_prompt_line(context_after[i].jp) + "\n";
        }
        prompt += "</context_after>\n\n";
    }

    prompt +=
        "Translate the " + std::to_string(batch_lines.size()) +
        " lines inside <lines_to_translate> to English. ";
    if (has_prev_ctx && has_after_ctx) {
        prompt +=
            "Use <previous_context> for scene continuity and "
            "<context_after> for lookahead context, but do NOT translate them. ";
    } else if (has_prev_ctx) {
        prompt +=
            "Use <previous_context> for scene continuity, "
            "but do NOT translate it. ";
    } else if (has_after_ctx) {
        prompt +=
            "Use <context_after> for lookahead context, "
            "but do NOT translate it. ";
    }
    prompt +=
        "Output EXACTLY " + std::to_string(batch_lines.size()) +
        " numbered translations, one per line. "
        "Do NOT include [SPEAKER] tags in your output.";
    return prompt;
}

ParsedNumberedAnswer parse_numbered_response_strict(
    const std::string& response_text, int expected) {
    ParsedNumberedAnswer answer;
    if (response_text.empty()) {
        return answer;
    }

    const std::string unwrapped = unwrap_outer_markdown_fence(response_text);

    struct PhysicalLine {
        std::string trimmed;
        bool terminated_by_newline = false;
    };
    std::vector<PhysicalLine> lines;
    std::size_t pos = 0;
    while (pos < unwrapped.size()) {
        const std::size_t nl = unwrapped.find('\n', pos);
        const bool has_nl = (nl != std::string::npos);
        const std::string raw_slice =
            has_nl ? unwrapped.substr(pos, nl - pos) : unwrapped.substr(pos);
        pos = has_nl ? (nl + 1U) : unwrapped.size();

        std::string t = trim(raw_slice);
        if (!t.empty()) {
            lines.push_back(PhysicalLine{std::move(t), has_nl});
        }
    }

    if (lines.empty()) {
        return answer;
    }

    if (expected <= 0) {
        answer.rejected = static_cast<int>(lines.size());
        return answer;
    }

    int expected_next = 1;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const bool is_last_unterminated =
            (i + 1U == lines.size() && !lines[i].terminated_by_newline);
        const std::string candidate_line =
            trim(strip_known_speaker_tags(lines[i].trimmed));
        const ParsedLineOpener parsed = parse_single_numbered_line(
            candidate_line.empty() ? lines[i].trimmed : candidate_line);

        if (parsed.kind == LineParseKind::IncompleteOpenerOnly) {
            if (is_last_unterminated &&
                !answer.entries.empty() &&
                parsed.number == expected_next &&
                parsed.number <= expected) {
                answer.final_tail = lines[i].trimmed;
                continue;
            }
            ++answer.rejected;
            continue;
        }

        if (parsed.kind == LineParseKind::MalformedOpener) {
            ++answer.rejected;
            continue;
        }

        if (parsed.kind == LineParseKind::ContinuationOrOther) {
            if (answer.entries.empty()) {
                ++answer.rejected;
                continue;
            }
            const std::string cont = normalize_parsed_body(lines[i].trimmed);
            if (!cont.empty()) {
                answer.entries.rbegin()->second += " " + cont;
            }
            continue;
        }

        if (parsed.number < 1 ||
            parsed.number > expected ||
            answer.entries.count(parsed.number) != 0 ||
            parsed.number != expected_next) {
            ++answer.rejected;
            continue;
        }

        std::string normalized = normalize_parsed_body(parsed.body);
        if (normalized.empty()) {
            ++answer.rejected;
            continue;
        }
        answer.entries.emplace(parsed.number, std::move(normalized));
        ++expected_next;
    }

    if (answer.rejected > 0) {
        answer.entries.clear();
    }
    return answer;
}

std::map<int, std::string> extract_usable_translations(
    const GeminiResponse& response,
    std::size_t requested,
    std::string* warning_out) {
    if (warning_out != nullptr) {
        warning_out->clear();
    }
    if (requested == 0) {
        return {};
    }
    if (response.content_blocked) {
        if (warning_out != nullptr) {
            *warning_out = "response blocked (" +
                           (response.finish_reason.empty()
                                ? std::string(kDefaultBlockReason)
                                : response.finish_reason) +
                           ")";
        }
        return {};
    }

    constexpr auto kMaxIntSize =
        static_cast<std::size_t>(std::numeric_limits<int>::max());
    const int expected = static_cast<int>(std::min(requested, kMaxIntSize));
    ParsedNumberedAnswer parsed =
        parse_numbered_response_strict(response.text, expected);

    if (parsed.rejected > 0) {
        if (warning_out != nullptr) {
            *warning_out = "rejected " + std::to_string(parsed.rejected) +
                           " malformed or out-of-order numbered line(s)";
        }
        return {};
    }

    if (response.finish_reason == kMaxTokensFinishReason) {
        if (parsed.entries.empty()) {
            if (warning_out != nullptr) {
                *warning_out = "MAX_TOKENS truncated before completing line 1";
            }
            return {};
        }
        if (parsed.final_tail.empty()) {
            parsed.entries.erase(std::prev(parsed.entries.end()));
        }
        if (warning_out != nullptr) {
            *warning_out = "MAX_TOKENS reached; salvaged " +
                           std::to_string(parsed.entries.size()) + "/" +
                           std::to_string(requested) +
                           " complete lines and re-queued tail";
        }
        return parsed.entries;
    }

    if (!parsed.final_tail.empty() || parsed.entries.size() != requested) {
        if (warning_out != nullptr) {
            *warning_out = "expected " + std::to_string(requested) +
                           " numbered translations, got " +
                           std::to_string(parsed.entries.size());
        }
        return {};
    }

    return parsed.entries;
}

bool validate_line_translation(const std::string& jp,
                               const std::string& en,
                               std::string* reason_out) {
    if (reason_out != nullptr) {
        reason_out->clear();
    }

    const std::string trimmed_en = trim(en);
    const std::string en_body = trim(strip_speaker_plate_body(jp, en));
    if (trimmed_en.empty() || en_body.empty()) {
        if (reason_out != nullptr) {
            *reason_out = "empty translation";
        }
        return false;
    }

    const std::string trimmed_jp = trim(jp);
    const std::string jp_body = trim(strip_speaker_plate_body(jp, jp));
    if ((has_non_ascii_bytes(jp) &&
         (en == jp || trimmed_en == trimmed_jp)) ||
        (!jp_body.empty() && has_non_ascii_bytes(jp_body) &&
         en_body == jp_body)) {
        if (reason_out != nullptr) {
            *reason_out = "unchanged Japanese echo";
        }
        return false;
    }

    if (contains_leaked_kana_or_kanji(en)) {
        if (reason_out != nullptr) {
            *reason_out = "leaked Japanese kana/kanji in translation";
        }
        return false;
    }

    const std::size_t jp_pct_n = count_non_overlapping(jp, kEngineLineBreakToken);
    if (jp_pct_n > 0) {
        const std::size_t en_pct_n =
            count_non_overlapping(en, kEngineLineBreakToken);
        if (en_pct_n != jp_pct_n) {
            if (reason_out != nullptr) {
                *reason_out = "mismatched %N line-break count (expected " +
                              std::to_string(jp_pct_n) + ", got " +
                              std::to_string(en_pct_n) + ")";
            }
            return false;
        }
    }

    return true;
}

double estimate_cost_usd(const TokenUsage& usage, const std::string& model) {
    const std::int64_t prompt = std::max<std::int64_t>(0, usage.prompt_tokens);
    const std::int64_t cached = std::max<std::int64_t>(0, usage.cached_tokens);
    const std::int64_t uncached_input = std::max<std::int64_t>(0, prompt - cached);
    const std::int64_t total_output =
        std::max<std::int64_t>(0, usage.output_tokens) +
        std::max<std::int64_t>(0, usage.thought_tokens);

    const double output_rate = is_gemini_31_pro_model(model)
                                   ? kGemini31ProOutputCostPerMillionUsd
                                   : kArgonOutputCostPerMillionUsd;

    return (static_cast<double>(uncached_input) / kTokensPerMillion) *
               kInputCostPerMillionUsd +
           (static_cast<double>(cached) / kTokensPerMillion) *
               kCachedInputCostPerMillionUsd +
           (static_cast<double>(total_output) / kTokensPerMillion) * output_rate;
}

double estimate_cost_eur(const TokenUsage& usage, const std::string& model) {
    return estimate_cost_usd(usage, model) * kUsdToEurConservativeFactor;
}

int translate_all(const TranslateOptions& opt) {
    if (opt.input_file.empty()) {
        throw std::runtime_error("translate_all: input_file must not be empty");
    }

    if (opt.test_batches > 0 && !opt.cache_file.empty()) {
        if (const auto refusal = refuse_cache_discard(
                cache_entry_count(opt.cache_file), "--test", false)) {
            log_info("[ERROR] " + *refusal);
            return kExitGateOrRefused;
        }
    }

    bj::value all_data_v = json_parse_file(opt.input_file);
    if (!all_data_v.is_object()) {
        throw std::runtime_error("extracted text root must be a JSON object: " +
                                 opt.input_file);
    }
    const bj::object& all_data = all_data_v.get_object();

    log_info("\nRunning speaker gate...");
    const SpeakerCheckReport gate = run_speaker_checks(all_data);
    if (!gate.passed) {
        log_info("\n[ERROR] speaker check " +
                 std::to_string(gate.failed_check) + " failed: " + gate.reason);
        log_info("        Nothing was translated and nothing was written.");
        return kExitGateOrRefused;
    }

    Cache cache = opt.cache_file.empty() ? Cache{} : load_cache(opt.cache_file);
    purge_failed_entries(cache);
    seed_glossary_entries(cache);

    const std::vector<std::string> file_order = build_ordered_file_list(all_data);
    const std::size_t max_uncached_limit =
        opt.test_batches > 0
            ? static_cast<std::size_t>(opt.test_batches) *
                  static_cast<std::size_t>(kDefaultBatchSize)
            : std::numeric_limits<std::size_t>::max();

    std::unordered_set<std::string> missing_unique;
    for (const std::string& fname : file_order) {
        const auto* fval = all_data.if_contains(fname);
        if (fval == nullptr || !fval->is_object()) {
            continue;
        }
        const std::vector<StreamEntry> stream =
            collect_file_translatable_entries(fval->get_object());
        for (const StreamEntry& entry : stream) {
            if (!cache.contains(entry.jp)) {
                missing_unique.insert(entry.jp);
                if (opt.test_batches > 0 &&
                    missing_unique.size() >= max_uncached_limit) {
                    break;
                }
            }
        }
        if (opt.test_batches > 0 &&
            missing_unique.size() >= max_uncached_limit) {
            break;
        }
    }

    if (!missing_unique.empty()) {
        throw std::runtime_error(
            "Offline build: " +
            comma(static_cast<long long>(missing_unique.size())) +
            " unique translatable string(s) missing from cache" +
            (opt.cache_file.empty() ? "" : (" (" + opt.cache_file + ")")) +
            "; run 02_translate.exe first.");
    }

    if (!opt.cache_file.empty()) {
        save_cache(cache, opt.cache_file);
    }

    if (!opt.output_file.empty()) {
        write_translated_document_atomic(all_data_v, cache, opt.output_file);
    }

    if (!opt.tsv_file.empty() && !opt.output_file.empty()) {
        return build_tsv::run_build(
            opt.output_file, opt.tsv_file, opt.game_tsv_file);
    }

    return kExitOk;
}

std::optional<std::string> validate_gemini_options(
    const GeminiTranslateOptions& opt) {
    if (opt.batch_size < 1) {
        return "--batch must be at least 1 (got " +
               std::to_string(opt.batch_size) + ")";
    }
    if (opt.max_batches < 0) {
        return "--max-batches cannot be negative (got " +
               std::to_string(opt.max_batches) + ")";
    }
    if (opt.context_window < 0) {
        return "--context cannot be negative (got " +
               std::to_string(opt.context_window) + ")";
    }
    if (opt.context_after_window < 0) {
        return "--context-after cannot be negative (got " +
               std::to_string(opt.context_after_window) + ")";
    }
    if (opt.input_file.empty()) {
        return "input_file must not be empty";
    }
    return std::nullopt;
}

struct BatchTranslator::Impl {
    ITranslationTransport* injected_transport = nullptr;
    std::unique_ptr<GeminiClient> owned_client;

    explicit Impl(ITranslationTransport* transport)
        : injected_transport(transport) {}

    ITranslationTransport& ensure_transport(const std::string& active_model) {
        if (injected_transport != nullptr) {
            return *injected_transport;
        }
        if (!owned_client) {
            GeminiClientOptions client_opts;
            client_opts.model = active_model;
            owned_client = std::make_unique<GeminiClient>(std::move(client_opts));
        }
        return *owned_client;
    }

    TranslateReport run(const GeminiTranslateOptions& opt) {
        TranslateReport report;

        if (const auto invalid_reason = validate_gemini_options(opt)) {
            log_info("[ERROR] " + *invalid_reason);
            report.exit_code = kExitFailure;
            return report;
        }

        const std::string active_model =
            opt.model.empty() ? std::string(kDefaultModel) : opt.model;
        owned_client.reset();

        bj::value all_data_v = json_parse_file(opt.input_file);
        if (!all_data_v.is_object()) {
            log_info("[ERROR] extracted text root must be a JSON object: " +
                     opt.input_file);
            report.exit_code = kExitFailure;
            return report;
        }
        bj::object& all_data = all_data_v.get_object();

        log_info("\nRunning speaker gate...");
        const SpeakerCheckReport gate = run_speaker_checks(all_data);
        if (!gate.passed) {
            log_info("\n[ERROR] speaker check " +
                     std::to_string(gate.failed_check) + " failed: " +
                     gate.reason);
            log_info("        Nothing was translated and nothing was written.");
            report.exit_code = kExitGateOrRefused;
            return report;
        }

        const std::size_t total_extracted_entries =
            count_total_extracted_entries(all_data);
        const std::vector<std::string> file_order =
            build_ordered_file_list(all_data);
        std::vector<std::pair<std::string, std::vector<StreamEntry>>> file_streams;
        file_streams.reserve(file_order.size());

        std::unordered_set<std::string> all_unique_jp;
        for (const std::string& fname : file_order) {
            const auto* fval = all_data.if_contains(fname);
            if (fval == nullptr || !fval->is_object()) {
                continue;
            }
            std::vector<StreamEntry> stream =
                collect_file_translatable_entries(fval->get_object());
            report.total_translatable += stream.size();
            for (const StreamEntry& e : stream) {
                all_unique_jp.insert(e.jp);
            }
            file_streams.emplace_back(fname, std::move(stream));
        }
        report.unique_translatable = all_unique_jp.size();

        auto count_remaining_uncached = [&](const Cache& c) -> std::size_t {
            std::size_t count = 0;
            for (const std::string& jp : all_unique_jp) {
                if (!c.contains(jp)) {
                    ++count;
                }
            }
            return count;
        };

        if (opt.gate_only) {
            Cache preview_cache;
            if (!(opt.retranslate && opt.discard_cache) &&
                !opt.cache_file.empty()) {
                preview_cache = load_cache(opt.cache_file);
                purge_failed_entries(preview_cache);
            }
            seed_glossary_entries(preview_cache);
            report.cached_total = preview_cache.size();
            report.remaining_uncached = count_remaining_uncached(preview_cache);
            const auto bsz = static_cast<std::size_t>(opt.batch_size);
            const std::size_t est_batches =
                (report.remaining_uncached + bsz - 1U) / bsz;

            log_info("\nGate-only summary (0 API calls, 0 files written):");
            log_info("  Total extracted entries:   " +
                     comma(static_cast<long long>(total_extracted_entries)));
            log_info("  Total translatable lines:  " +
                     comma(static_cast<long long>(report.total_translatable)));
            log_info("  Unique translatable lines: " +
                     comma(static_cast<long long>(report.unique_translatable)));
            log_info("  Cached entries (before):   " +
                     comma(static_cast<long long>(report.cached_total)));
            log_info("  Remaining uncached unique: " +
                     comma(static_cast<long long>(report.remaining_uncached)) +
                     " (~" + std::to_string(est_batches) + " batches)");
            report.exit_code = kExitOk;
            return report;
        }

        if (opt.retranslate) {
            const std::size_t existing_entries =
                opt.cache_file.empty() ? 0U : cache_entry_count(opt.cache_file);
            if (const auto refusal = refuse_cache_discard(
                    existing_entries, "--retranslate", opt.discard_cache)) {
                log_info("[ERROR] " + *refusal);
                report.exit_code = kExitGateOrRefused;
                return report;
            }
            if (injected_transport == nullptr &&
                GeminiClient::resolve_gemini_api_key().empty()) {
                log_info(
                    "[ERROR] GEMINI_API_KEY not set -- refusing to discard the "
                    "cache for a run that cannot translate.");
                report.exit_code = kExitFailure;
                return report;
            }
            if (!remove_file_if_present(opt.cache_file) ||
                !remove_file_if_present(opt.blocked_file)) {
                report.exit_code = kExitFailure;
                return report;
            }
        }

        Cache cache;
        if (!opt.retranslate && !opt.cache_file.empty()) {
            cache = load_cache(opt.cache_file);
            const std::size_t purged = purge_failed_entries(cache);
            log_info("Loaded cache: " + std::to_string(cache.size()) +
                     " entries" +
                     (purged > 0
                          ? (" (purged " + std::to_string(purged) +
                             " failed echoes)")
                          : ""));
        }

        seed_glossary_entries(cache);

        std::vector<BlockedLine> blocked_lines =
            opt.retranslate ? std::vector<BlockedLine>{}
                            : load_existing_blocked_lines(opt.blocked_file);
        blocked_lines.erase(
            std::remove_if(blocked_lines.begin(),
                           blocked_lines.end(),
                           [&](const BlockedLine& b) {
                               return cache.contains(b.jp);
                           }),
            blocked_lines.end());

        std::unordered_set<std::string> blocked_this_run;

        auto erase_blocked_by_jp = [&](const std::string& jp) {
            const auto prev_sz = blocked_lines.size();
            blocked_lines.erase(
                std::remove_if(blocked_lines.begin(),
                               blocked_lines.end(),
                               [&](const BlockedLine& b) { return b.jp == jp; }),
                blocked_lines.end());
            if (blocked_lines.size() != prev_sz) {
                save_blocked_lines_atomic(opt.blocked_file, blocked_lines);
            }
        };

        auto record_blocked_leaf = [&](const PromptLine& item,
                                       const std::string& reason_raw) {
            const std::string reason =
                reason_raw.empty() ? std::string(kDefaultBlockReason) : reason_raw;
            blocked_this_run.insert(item.jp);
            for (BlockedLine& existing : blocked_lines) {
                if (existing.jp == item.jp) {
                    existing.offset = item.offset;
                    existing.speaker = item.speaker;
                    existing.reason = reason;
                    save_blocked_lines_atomic(opt.blocked_file, blocked_lines);
                    return;
                }
            }
            blocked_lines.push_back(
                BlockedLine{item.offset, item.speaker, item.jp, reason});
            save_blocked_lines_atomic(opt.blocked_file, blocked_lines);
        };

        const std::size_t initial_uncached = count_remaining_uncached(cache);
        const auto batch_sz = static_cast<std::size_t>(opt.batch_size);
        std::size_t planned_total_batches =
            (initial_uncached + batch_sz - 1U) / batch_sz;
        if (opt.max_batches > 0 &&
            planned_total_batches > static_cast<std::size_t>(opt.max_batches)) {
            planned_total_batches = static_cast<std::size_t>(opt.max_batches);
        }

        log_info("Corpus summary: entries=" +
                 comma(static_cast<long long>(total_extracted_entries)) +
                 " translatable=" +
                 comma(static_cast<long long>(report.total_translatable)) +
                 " unique=" +
                 comma(static_cast<long long>(report.unique_translatable)) +
                 " uncached=" + comma(static_cast<long long>(initial_uncached)) +
                 " batches=" + std::to_string(planned_total_batches));

        int top_level_batches_done = 0;
        std::int64_t calls_in_current_batch = 0;

        auto budget_exhausted = [&]() noexcept -> bool {
            return opt.max_budget_eur > 0.0 &&
                   estimate_cost_eur(report.usage, active_model) >=
                       opt.max_budget_eur;
        };

        auto batch_call_cap_reached = [&]() noexcept -> bool {
            return opt.max_batches > 0 &&
                   calls_in_current_batch >= kMaxCallsPerBoundedBatch;
        };

        auto invoke_single_call =
            [&](const std::vector<PromptLine>& slice,
                const std::vector<std::pair<std::string, std::string>>& prev_ctx,
                const std::vector<PromptLine>& after_ctx,
                bool use_neutral) -> std::optional<GeminiResponse> {
            if (budget_exhausted()) {
                report.stopped_by_budget = true;
                return std::nullopt;
            }
            if (batch_call_cap_reached()) {
                return std::nullopt;
            }

            GeminiRequest req;
            req.system_prompt =
                use_neutral ? neutral_system_prompt() : system_prompt();
            req.user_prompt = build_batch_user_prompt(
                slice, prev_ctx, after_ctx, opt.context_window,
                opt.context_after_window);
            req.max_output_tokens =
                compute_max_output_tokens(static_cast<int>(slice.size()));
            req.thinking_budget = opt.thinking_budget;
            req.thinking_level = opt.thinking_level;
            req.safety_block_none = true;

            GeminiResponse resp = ensure_transport(active_model).generate(req);
            ++calls_in_current_batch;
            ++report.usage.api_calls;
            report.usage.prompt_tokens += resp.prompt_tokens;
            report.usage.cached_tokens += resp.cached_tokens;
            report.usage.output_tokens += resp.output_tokens;
            report.usage.thought_tokens += resp.thought_tokens;

            append_raw_answer_log(opt.raw_log_file,
                                  top_level_batches_done + 1,
                                  slice.size(),
                                  use_neutral,
                                  resp);
            return resp;
        };

        auto apply_usable_from_response =
            [&](const std::vector<PromptLine>& slice,
                const GeminiResponse& resp) -> std::size_t {
            std::string warn;
            const std::map<int, std::string> usable =
                extract_usable_translations(resp, slice.size(), &warn);
            if (!warn.empty()) {
                log_info("    WARN: " + warn);
            }
            std::size_t applied = 0;
            for (const auto& [one_based_idx, parsed_en] : usable) {
                if (one_based_idx < 1 ||
                    static_cast<std::size_t>(one_based_idx) > slice.size()) {
                    continue;
                }
                const PromptLine& item =
                    slice[static_cast<std::size_t>(one_based_idx - 1)];
                const std::string cleaned = clean_translated_line_impl(
                    item.jp, parsed_en, /*unescape_newlines=*/false);
                std::string reject_reason;
                if (!validate_line_translation(item.jp, cleaned, &reject_reason)) {
                    log_info("    Rejected line " +
                             std::to_string(one_based_idx) + " (" +
                             reject_reason + ") -- keeping uncached");
                    continue;
                }
                if (!cache.contains(item.jp)) {
                    ++report.translated_new;
                }
                cache.set(item.jp, cleaned);
                erase_blocked_by_jp(item.jp);
                ++applied;
            }
            return applied;
        };

        auto collect_still_pending =
            [&](const std::vector<PromptLine>& slice) -> std::vector<PromptLine> {
            std::vector<PromptLine> pending;
            for (const PromptLine& item : slice) {
                if (!cache.contains(item.jp) &&
                    blocked_this_run.count(item.jp) == 0) {
                    pending.push_back(item);
                }
            }
            return pending;
        };

        auto translate_neutral_bisect =
            [&](auto& self_bisect,
                const std::vector<PromptLine>& sub_slice,
                auto& recover_fn) -> void {
            if (sub_slice.empty() || report.stopped_by_budget ||
                batch_call_cap_reached()) {
                return;
            }
            const auto resp_opt =
                invoke_single_call(sub_slice, {}, {}, /*use_neutral=*/true);
            if (!resp_opt.has_value()) {
                return;
            }
            if (!resp_opt->content_blocked) {
                recover_fn(recover_fn, sub_slice, *resp_opt,
                           /*use_neutral=*/true, 0);
                return;
            }
            if (sub_slice.size() == 1U) {
                log_info("    Blocked single line at offset " +
                         std::to_string(sub_slice[0].offset) + " (" +
                         resp_opt->finish_reason + ") -- recording to blocked_lines");
                record_blocked_leaf(sub_slice[0], resp_opt->finish_reason);
                return;
            }
            const std::size_t mid = sub_slice.size() / kBinarySplitDivisor;
            const std::vector<PromptLine> left(
                sub_slice.begin(),
                sub_slice.begin() + static_cast<std::ptrdiff_t>(mid));
            const std::vector<PromptLine> right(
                sub_slice.begin() + static_cast<std::ptrdiff_t>(mid),
                sub_slice.end());
            self_bisect(self_bisect, left, recover_fn);
            self_bisect(self_bisect, right, recover_fn);
        };

        auto recover_incomplete_slice =
            [&](auto& self_recover,
                const std::vector<PromptLine>& slice,
                const GeminiResponse& resp,
                bool use_neutral,
                int retry_depth) -> void {
            const std::size_t applied = apply_usable_from_response(slice, resp);
            const std::vector<PromptLine> pending = collect_still_pending(slice);
            if (pending.empty() || report.stopped_by_budget ||
                batch_call_cap_reached() ||
                retry_depth >= kMaxIncompleteRetries) {
                return;
            }

            auto retry_one_subslice = [&](const std::vector<PromptLine>& sub) {
                if (sub.empty() || report.stopped_by_budget ||
                    batch_call_cap_reached()) {
                    return;
                }
                const auto sub_resp =
                    invoke_single_call(sub, {}, {}, use_neutral);
                if (!sub_resp.has_value()) {
                    return;
                }
                if (sub_resp->content_blocked) {
                    translate_neutral_bisect(
                        translate_neutral_bisect, sub, self_recover);
                    return;
                }
                self_recover(self_recover, sub, *sub_resp, use_neutral,
                             retry_depth + 1);
            };

            if (applied == 0U && pending.size() > 1U) {
                const std::size_t mid = pending.size() / kBinarySplitDivisor;
                const std::vector<PromptLine> left(
                    pending.begin(),
                    pending.begin() + static_cast<std::ptrdiff_t>(mid));
                const std::vector<PromptLine> right(
                    pending.begin() + static_cast<std::ptrdiff_t>(mid),
                    pending.end());
                retry_one_subslice(left);
                retry_one_subslice(right);
                return;
            }

            retry_one_subslice(pending);
        };

        const std::size_t max_retained_ctx = std::max(
            kMinRetainedContextLines,
            static_cast<std::size_t>(opt.context_window) *
                kContextRetentionMultiplier);

        if (initial_uncached > 0) {
            for (const auto& [fname, stream] : file_streams) {
                (void)fname;
                std::vector<std::pair<std::string, std::string>> previous_context;
                std::size_t cursor = 0;

                while (cursor < stream.size()) {
                    while (cursor < stream.size()) {
                        const StreamEntry& cur = stream[cursor];
                        if (const std::string* en = cache.get(cur.jp)) {
                            previous_context.emplace_back(
                                cur.speaker,
                                strip_speaker_plate_body(cur.jp, *en));
                            ++cursor;
                            continue;
                        }
                        if (blocked_this_run.count(cur.jp) != 0) {
                            ++cursor;
                            continue;
                        }
                        break;
                    }

                    if (previous_context.size() > max_retained_ctx) {
                        previous_context.erase(
                            previous_context.begin(),
                            previous_context.end() -
                                static_cast<std::ptrdiff_t>(max_retained_ctx));
                    }

                    if (cursor >= stream.size()) {
                        break;
                    }

                    if (budget_exhausted()) {
                        report.stopped_by_budget = true;
                        break;
                    }
                    if (opt.max_batches > 0 &&
                        top_level_batches_done >= opt.max_batches) {
                        report.stopped_by_max_batches = true;
                        break;
                    }

                    const std::size_t window_begin = cursor;
                    std::vector<PromptLine> batch_lines;
                    batch_lines.reserve(batch_sz);
                    std::unordered_set<std::string> seen_in_batch;

                    while (cursor < stream.size() &&
                           batch_lines.size() < batch_sz) {
                        const StreamEntry& cur = stream[cursor];
                        if (!cache.contains(cur.jp) &&
                            blocked_this_run.count(cur.jp) == 0 &&
                            seen_in_batch.insert(cur.jp).second) {
                            batch_lines.push_back(
                                PromptLine{cur.speaker, cur.jp, cur.offset});
                        }
                        ++cursor;
                    }

                    while (cursor < stream.size() &&
                           seen_in_batch.count(stream[cursor].jp) != 0) {
                        ++cursor;
                    }
                    const std::size_t window_end = cursor;

                    if (batch_lines.empty()) {
                        continue;
                    }

                    std::vector<PromptLine> context_after;
                    if (opt.context_after_window > 0 &&
                        window_end < stream.size()) {
                        const std::size_t after_end = std::min(
                            stream.size(),
                            window_end +
                                static_cast<std::size_t>(opt.context_after_window));
                        context_after.reserve(after_end - window_end);
                        for (std::size_t a = window_end; a < after_end; ++a) {
                            if (blocked_this_run.count(stream[a].jp) != 0) {
                                continue;
                            }
                            context_after.push_back(PromptLine{
                                stream[a].speaker, stream[a].jp,
                                stream[a].offset});
                        }
                    }

                    calls_in_current_batch = 0;

                    auto resp_opt = invoke_single_call(
                        batch_lines, previous_context, context_after,
                        /*use_neutral=*/false);
                    if (!resp_opt.has_value()) {
                        break;
                    }

                    bool used_neutral = false;
                    if (resp_opt->content_blocked &&
                        ((opt.context_window > 0 && !previous_context.empty()) ||
                         !context_after.empty())) {
                        log_info("    Blocked with context (" +
                                 resp_opt->finish_reason +
                                 ") -- retrying without context...");
                        resp_opt = invoke_single_call(
                            batch_lines, {}, {}, /*use_neutral=*/false);
                        if (!resp_opt.has_value()) {
                            break;
                        }
                    }

                    if (resp_opt->content_blocked) {
                        log_info("    Blocked (" + resp_opt->finish_reason +
                                 ") -- retrying with neutral system prompt...");
                        used_neutral = true;
                        resp_opt = invoke_single_call(
                            batch_lines, {}, {}, /*use_neutral=*/true);
                        if (!resp_opt.has_value()) {
                            break;
                        }
                    }

                    if (resp_opt->content_blocked) {
                        if (batch_lines.size() == 1U) {
                            record_blocked_leaf(
                                batch_lines[0], resp_opt->finish_reason);
                        } else {
                            log_info("    Batch still blocked (" +
                                     resp_opt->finish_reason +
                                     ") -- bisecting " +
                                     std::to_string(batch_lines.size()) +
                                     " lines...");
                            const std::size_t mid =
                                batch_lines.size() / kBinarySplitDivisor;
                            const std::vector<PromptLine> left(
                                batch_lines.begin(),
                                batch_lines.begin() +
                                    static_cast<std::ptrdiff_t>(mid));
                            const std::vector<PromptLine> right(
                                batch_lines.begin() +
                                    static_cast<std::ptrdiff_t>(mid),
                                batch_lines.end());
                            translate_neutral_bisect(
                                translate_neutral_bisect, left,
                                recover_incomplete_slice);
                            translate_neutral_bisect(
                                translate_neutral_bisect, right,
                                recover_incomplete_slice);
                        }
                    } else {
                        recover_incomplete_slice(recover_incomplete_slice,
                                                 batch_lines, *resp_opt,
                                                 used_neutral, 0);
                    }

                    std::size_t resolved_in_batch = 0;
                    for (const PromptLine& item : batch_lines) {
                        if (cache.contains(item.jp)) {
                            ++resolved_in_batch;
                        }
                    }

                    for (std::size_t k = window_begin; k < window_end; ++k) {
                        const StreamEntry& w = stream[k];
                        if (const std::string* en = cache.get(w.jp)) {
                            previous_context.emplace_back(
                                w.speaker, strip_speaker_plate_body(w.jp, *en));
                        }
                    }

                    ++top_level_batches_done;
                    if (!opt.cache_file.empty()) {
                        save_cache(cache, opt.cache_file);
                    }
                    write_last_batch_snapshot(
                        opt.last_batch_file,
                        top_level_batches_done,
                        static_cast<int>(std::max<std::size_t>(
                            planned_total_batches,
                            static_cast<std::size_t>(top_level_batches_done))),
                        cache.size(),
                        batch_lines,
                        cache);

                    const double cum_usd =
                        estimate_cost_usd(report.usage, active_model);
                    const double cum_eur =
                        estimate_cost_eur(report.usage, active_model);
                    char telemetry[kTelemetryBufferBytes];
                    std::snprintf(
                        telemetry, sizeof(telemetry),
                        "[Batch %d/%zu] lines=%zu/%zu cache_total=%zu | "
                        "tokens in=%lld cached=%lld out=%lld thought=%lld | "
                        "cost=$%.4f (EUR %.4f)",
                        top_level_batches_done,
                        std::max<std::size_t>(
                            planned_total_batches,
                            static_cast<std::size_t>(top_level_batches_done)),
                        resolved_in_batch,
                        batch_lines.size(),
                        cache.size(),
                        static_cast<long long>(report.usage.prompt_tokens),
                        static_cast<long long>(report.usage.cached_tokens),
                        static_cast<long long>(report.usage.output_tokens),
                        static_cast<long long>(report.usage.thought_tokens),
                        cum_usd,
                        cum_eur);
                    log_info(telemetry);

                    if (budget_exhausted() &&
                        count_remaining_uncached(cache) > 0) {
                        report.stopped_by_budget = true;
                    }
                    if (report.stopped_by_budget) {
                        break;
                    }
                }

                if (report.stopped_by_budget || report.stopped_by_max_batches) {
                    break;
                }
            }
        } else if (!opt.cache_file.empty()) {
            save_cache(cache, opt.cache_file);
        }

        report.cached_total = cache.size();
        report.blocked_count = blocked_lines.size();
        report.remaining_uncached = count_remaining_uncached(cache);
        report.estimated_usd = estimate_cost_usd(report.usage, active_model);
        report.estimated_eur = estimate_cost_eur(report.usage, active_model);

        log_final_translation_summary(active_model, report);

        const bool stopped_early =
            report.stopped_by_max_batches || report.stopped_by_budget;
        if (stopped_early && opt.skip_tsv_on_partial) {
            log_info(
                "Partial run stopped early -- keeping cache updated and "
                "skipping translated_text.json / translations.tsv rebuild.");
            report.exit_code = kExitOk;
            return report;
        }

        if (!opt.output_file.empty() &&
            (report.remaining_uncached == 0 || !opt.skip_tsv_on_partial)) {
            write_translated_document_atomic(
                all_data_v, cache, opt.output_file);
            if (!opt.tsv_file.empty()) {
                report.exit_code = build_tsv::run_build(
                    opt.output_file, opt.tsv_file, opt.game_tsv_file);
                return report;
            }
        }

        report.exit_code = kExitOk;
        return report;
    }
};

BatchTranslator::BatchTranslator(ITranslationTransport* transport_override)
    : impl_(std::make_unique<Impl>(transport_override)) {}

BatchTranslator::~BatchTranslator() = default;
BatchTranslator::BatchTranslator(BatchTranslator&&) noexcept = default;
BatchTranslator& BatchTranslator::operator=(BatchTranslator&&) noexcept = default;

TranslateReport BatchTranslator::run(const GeminiTranslateOptions& opt) {
    return impl_->run(opt);
}

}  // namespace crc::translate
