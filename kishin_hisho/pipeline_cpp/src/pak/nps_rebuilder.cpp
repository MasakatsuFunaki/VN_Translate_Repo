// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "pak/nps_rebuilder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <optional>
#include <set>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

#include <boost/json.hpp>
#include <boost/regex.hpp>

#include "common/util.h"
#include "patch/deploy.h"

namespace fs = std::filesystem;

namespace ksh::pak {

namespace {

constexpr int kBoxType0 = 0;
constexpr int kBoxType1 = 1;
constexpr int kBoxType2 = 2;
constexpr int kBoxType3 = 3;
constexpr int kBoxType4 = 4;
constexpr int kBoxType5 = 5;
constexpr int kBoxType6 = 6;
constexpr int kBoxType7 = 7;
constexpr int kBoxType8 = 8;
constexpr int kBoxType9 = 9;
constexpr int kBoxType10 = 10;
constexpr int kBoxType11 = 11;
constexpr int kBoxType12 = 12;
constexpr int kBoxType13 = 13;

constexpr int kMaxFourLinePaginatedBoxType = 8;
constexpr std::size_t kMaxLinesPerFourLineBox = 4;

constexpr int kHalfwidthColumnWidth = 1;
constexpr int kFullwidthColumnWidth = 2;
constexpr int kMinTypewriterColumnBudget = 20;
constexpr char32_t kMaxAsciiCodepoint = 0x7F;

constexpr std::uint8_t kUtf8AsciiLimit = 0x80;
constexpr std::uint8_t kUtf8TwoByteLead = 0xC0;
constexpr std::uint8_t kUtf8ThreeByteLead = 0xE0;
constexpr std::uint8_t kUtf8FourByteLead = 0xF0;
constexpr std::uint8_t kUtf8ContinuationMask = 0x3F;
constexpr unsigned int kUtf8ContinuationShift = 6;

constexpr std::size_t kUtf8OneByteLen = 1;
constexpr std::size_t kUtf8TwoByteLen = 2;
constexpr std::size_t kUtf8ThreeByteLen = 3;
constexpr std::size_t kUtf8FourByteLen = 4;

constexpr std::size_t kCenteredIndentMinCodepoints = 2;
constexpr std::size_t kMaxPreservedFirstIndentCodepoints = 2;
constexpr std::size_t kEvenSegmentStep = 2;
constexpr std::size_t kMinDoubleHyphenLength = 2;

constexpr int kJsonReadMaxAttempts = 5;
constexpr int kJsonReadRetryDelayMs = 50;

constexpr std::string_view kFullwidthSpace = "\xE3\x80\x80";
constexpr std::string_view kHorizontalBar = "\xE2\x80\x95";
constexpr std::string_view kDoubleHorizontalBar = "\xE2\x80\x95\xE2\x80\x95";
constexpr std::string_view kNoBreakSpaceUtf8 = "\xC2\xA0";
constexpr std::string_view kNonNpsFilesKey = "__non_nps_files__";
constexpr std::string_view kTypewriterKeystrokeSeTag =
    R"(<SE ID="1" SRC="se人体行動_手動タイプ1文字打つ飛翔" MODE="normal" TIME="" LOOP="" vol="">)";

const boost::regex& SingleTagRegex() {
    static const boost::regex kRe(
        R"((?:<A\s+[^>]*OPERATOR="[^"]*"[^>]*>|</?[A-Za-z0-9_!][^>]*>))");
    return kRe;
}

const boost::regex& RubyWholeRegex() {
    static const boost::regex kRe(R"(<r\s+text="[^"]*"\s*>([\s\S]*?)</r>)",
                                  boost::regex::icase);
    return kRe;
}

const boost::regex& TrailingCloseOrWaitRegex() {
    static const boost::regex kRe(R"(^(?:</[A-Za-z0-9_]+>|<wait\b[^>]*>)*)",
                                  boost::regex::icase);
    return kRe;
}

const boost::regex& TagOrElemSplitRegex() {
    static const boost::regex kRe(
        R"((<r\b[^>]*>[\s\S]*?</r>|<FONT\b[^>]*>[\s\S]*?</FONT>|<[^>]+>))",
        boost::regex::icase);
    return kRe;
}

const boost::regex& FontSplitRegex() {
    static const boost::regex kRe(R"((<FONT\b[^>]*>[\s\S]*?</FONT>|<[^>]+>))",
                                  boost::regex::icase);
    return kRe;
}

const boost::regex& AnyTagSplitRegex() {
    static const boost::regex kRe(R"((<[^>]+>))");
    return kRe;
}

const boost::regex& LeadingWaitRegex() {
    static const boost::regex kRe(R"(^(?:<wait\b[^>]*>)*)", boost::regex::icase);
    return kRe;
}

const boost::regex& SentencePrefixRegex() {
    static const boost::regex kRe(R"(^[\s\S]*?[.!?]\s+)");
    return kRe;
}

const boost::regex& TrailingKRegex() {
    static const boost::regex kRe(R"(<[kK]>\s*$)");
    return kRe;
}

const boost::regex& TrailingTypewriterSeRegex() {
    static const boost::regex kRe(R"((<k>\r<SE ID="2"[^>]*>)$)", boost::regex::icase);
    return kRe;
}

const boost::regex& FontOpenRegex() {
    static const boost::regex kRe(R"(<FONT\b[^>]*>)", boost::regex::icase);
    return kRe;
}

const boost::regex& FontCloseRegex() {
    static const boost::regex kRe(R"(</FONT\s*>)", boost::regex::icase);
    return kRe;
}

const boost::regex& BoundaryPrefixFontRegex() {
    static const boost::regex kRe(R"(^(?:\xE3\x80\x80|\s)*(<FONT\b[^>]*>))",
                                  boost::regex::icase);
    return kRe;
}

const boost::regex& BoundarySuffixFontRegex() {
    static const boost::regex kRe(R"((\r?</FONT\s*>)\s*$)", boost::regex::icase);
    return kRe;
}

const boost::regex& LeadingIndentRegex() {
    static const boost::regex kRe(R"(^(?:\xE3\x80\x80|\s)*)");
    return kRe;
}

const boost::regex& MidWaitTime5Regex() {
    static const boost::regex kRe(R"(<wait\s+time="5"\s*>)", boost::regex::icase);
    return kRe;
}

const boost::regex& BoxType13Regex() {
    static const boost::regex kRe(R"(<BOX\s+type="13"\s*>)", boost::regex::icase);
    return kRe;
}

static std::size_t Utf8CodepointByteLength(std::string_view text, std::size_t pos) noexcept {
    const auto lead = static_cast<std::uint8_t>(text[pos]);
    std::size_t len = kUtf8OneByteLen;
    if (lead >= kUtf8FourByteLead) {
        len = kUtf8FourByteLen;
    } else if (lead >= kUtf8ThreeByteLead) {
        len = kUtf8ThreeByteLen;
    } else if (lead >= kUtf8TwoByteLead) {
        len = kUtf8TwoByteLen;
    }
    if (pos + len > text.size()) {
        return kUtf8OneByteLen;
    }
    return len;
}

static char32_t DecodeNextUtf8Codepoint(std::string_view text, std::size_t& pos) noexcept {
    const auto lead = static_cast<std::uint8_t>(text[pos]);
    if (lead < kUtf8AsciiLimit) {
        ++pos;
        return lead;
    }
    const std::size_t len = Utf8CodepointByteLength(text, pos);
    if (len == kUtf8OneByteLen) {
        ++pos;
        return lead;
    }
    const unsigned int initial_mask = (0x3Fu >> (len - kUtf8OneByteLen));
    char32_t cp = lead & initial_mask;
    for (std::size_t k = 1; k < len; ++k) {
        const auto cont = static_cast<std::uint8_t>(text[pos + k]);
        cp = (cp << kUtf8ContinuationShift) | (cont & kUtf8ContinuationMask);
    }
    pos += len;
    return cp;
}

static std::size_t CountUtf8Codepoints(std::string_view text) noexcept {
    std::size_t count = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
        pos += Utf8CodepointByteLength(text, pos);
        ++count;
    }
    return count;
}

static std::vector<std::string> SplitUtf8Codepoints(std::string_view text) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t len = Utf8CodepointByteLength(text, pos);
        out.emplace_back(text.substr(pos, len));
        pos += len;
    }
    return out;
}

static std::vector<std::string> SplitKeepingDelimiters(std::string_view text,
                                                       const boost::regex& pattern) {
    std::vector<std::string> parts;
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    const char* last = begin;
    boost::cmatch match;
    while (boost::regex_search(last, end, match, pattern)) {
        parts.emplace_back(last, match[0].first);
        parts.emplace_back(match[0].first, match[0].second);
        last = match[0].second;
    }
    parts.emplace_back(last, end);
    return parts;
}

static std::string JoinStrings(const std::vector<std::string>& items,
                               std::string_view separator) {
    if (items.empty()) {
        return {};
    }
    std::string out = items.front();
    for (std::size_t i = 1; i < items.size(); ++i) {
        out.append(separator);
        out.append(items[i]);
    }
    return out;
}

static std::string ReplaceAllLiteral(std::string_view text,
                                     std::string_view from,
                                     std::string_view to) {
    if (from.empty()) {
        return std::string(text);
    }
    std::string out;
    out.reserve(text.size());
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t pos = text.find(from, start);
        if (pos == std::string_view::npos) {
            out.append(text.substr(start));
            break;
        }
        out.append(text.substr(start, pos - start));
        out.append(to);
        start = pos + from.size();
    }
    return out;
}

static std::string ReplaceFirstLiteral(std::string_view text,
                                       std::string_view from,
                                       std::string_view to) {
    const std::size_t pos = text.find(from);
    if (pos == std::string_view::npos) {
        return std::string(text);
    }
    std::string out;
    out.reserve(text.size() - from.size() + to.size());
    out.append(text.substr(0, pos));
    out.append(to);
    out.append(text.substr(pos + from.size()));
    return out;
}

static bool EndsWithIgnoreCase(std::string_view text, std::string_view suffix) noexcept {
    if (text.size() < suffix.size()) {
        return false;
    }
    const std::size_t offset = text.size() - suffix.size();
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        char a = text[offset + i];
        char b = suffix[i];
        if (a >= 'a' && a <= 'z') {
            a = static_cast<char>(a - 'a' + 'A');
        }
        if (b >= 'a' && b <= 'z') {
            b = static_cast<char>(b - 'a' + 'A');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

static std::pair<std::string, std::vector<std::size_t>> StripTagsWithMap(std::string_view s) {
    std::string plain_text;
    std::vector<std::size_t> plain_to_s_end;
    plain_text.reserve(s.size());
    plain_to_s_end.reserve(s.size());

    const char* begin = s.data();
    const char* end = s.data() + s.size();
    std::size_t i = 0;
    while (i < s.size()) {
        boost::cmatch tag_match;
        if (boost::regex_search(begin + i, end, tag_match, SingleTagRegex(),
                                boost::match_continuous)) {
            i += static_cast<std::size_t>(tag_match[0].length());
            continue;
        }

        const std::size_t cp_len = Utf8CodepointByteLength(s, i);
        std::size_t end_pos = i + cp_len;
        boost::cmatch trail_match;
        if (boost::regex_search(begin + end_pos, end, trail_match, TrailingCloseOrWaitRegex(),
                                boost::match_continuous)) {
            end_pos += static_cast<std::size_t>(trail_match[0].length());
        }

        plain_text.append(s.substr(i, cp_len));
        for (std::size_t b = 0; b < cp_len; ++b) {
            plain_to_s_end.push_back(end_pos);
        }
        i += cp_len;
    }

    return {std::move(plain_text), std::move(plain_to_s_end)};
}

static std::string ReplaceHyphenRunsWithHorizontalBars(std::string_view segment) {
    std::string out;
    out.reserve(segment.size());
    std::size_t i = 0;
    while (i < segment.size()) {
        if (segment[i] != '-') {
            out.push_back(segment[i]);
            ++i;
            continue;
        }
        std::size_t run_end = i;
        while (run_end < segment.size() && segment[run_end] == '-') {
            ++run_end;
        }
        const std::size_t run_len = run_end - i;
        if (run_len >= kMinDoubleHyphenLength) {
            for (std::size_t k = 0; k < run_len; ++k) {
                out.append(kHorizontalBar);
            }
        } else {
            out.push_back('-');
        }
        i = run_end;
    }
    return out;
}

static std::string TrimSpaces(std::string_view text) {
    std::size_t start = 0;
    while (start < text.size()) {
        if (text[start] == ' ' || text[start] == '\t' || text[start] == '\r' ||
            text[start] == '\n') {
            ++start;
            continue;
        }
        if (text.substr(start).rfind(kFullwidthSpace, 0) == 0) {
            start += kFullwidthSpace.size();
            continue;
        }
        break;
    }

    std::size_t end = text.size();
    while (end > start) {
        if (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r' ||
            text[end - 1] == '\n') {
            --end;
            continue;
        }
        if (end >= start + kFullwidthSpace.size() &&
            text.substr(end - kFullwidthSpace.size(), kFullwidthSpace.size()) == kFullwidthSpace) {
            end -= kFullwidthSpace.size();
            continue;
        }
        break;
    }
    return std::string(text.substr(start, end - start));
}

static std::vector<std::string> SplitModifiedIntoClauses(std::string_view text, int target_count) {
    if (target_count <= 1) {
        return {std::string(text)};
    }

    std::vector<std::string> sentences;
    std::size_t seg_start = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        if ((text[i] == '.' || text[i] == '!' || text[i] == '?') && i + 1 < text.size() &&
            (text[i + 1] == ' ' || text[i + 1] == '\t' || text[i + 1] == '\r' ||
             text[i + 1] == '\n')) {
            const std::string trimmed = TrimSpaces(text.substr(seg_start, i + 1 - seg_start));
            if (!trimmed.empty()) {
                sentences.push_back(trimmed);
            }
            ++i;
            while (i < text.size() &&
                   (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) {
                ++i;
            }
            seg_start = i;
            continue;
        }
        ++i;
    }
    if (seg_start < text.size()) {
        const std::string tail = TrimSpaces(text.substr(seg_start));
        if (!tail.empty()) {
            sentences.push_back(tail);
        }
    }

    if (sentences.size() <= 1) {
        return {std::string(text)};
    }
    if (sentences.size() <= static_cast<std::size_t>(target_count)) {
        return sentences;
    }

    std::vector<std::string> out;
    const double per =
        static_cast<double>(sentences.size()) / static_cast<double>(target_count);
    for (int bucket = 0; bucket < target_count; ++bucket) {
        const auto start_idx =
            static_cast<std::size_t>(std::nearbyint(static_cast<double>(bucket) * per));
        const std::size_t end_idx =
            (bucket == target_count - 1)
                ? sentences.size()
                : static_cast<std::size_t>(std::nearbyint(static_cast<double>(bucket + 1) * per));
        if (start_idx >= end_idx || start_idx >= sentences.size()) {
            continue;
        }
        std::vector<std::string> slice(
            sentences.begin() + static_cast<std::ptrdiff_t>(start_idx),
            sentences.begin() +
                static_cast<std::ptrdiff_t>(std::min(end_idx, sentences.size())));
        const std::string chunk = TrimSpaces(JoinStrings(slice, " "));
        if (!chunk.empty()) {
            out.push_back(chunk);
        }
    }
    if (out.empty()) {
        return {std::string(text)};
    }
    return out;
}

static std::optional<std::size_t> FindCodepointSubsequence(
    const std::vector<std::string>& haystack,
    const std::vector<std::string>& needle) {
    if (needle.empty() || needle.size() > haystack.size()) {
        return std::nullopt;
    }
    const std::size_t limit = haystack.size() - needle.size();
    for (std::size_t i = 0; i <= limit; ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            if (haystack[i + j] != needle[j]) {
                match = false;
                break;
            }
        }
        if (match) {
            return i;
        }
    }
    return std::nullopt;
}

static std::string ReconstructTypewriter(std::string_view current_en,
                                         const NpsBlockMetadata& meta,
                                         int budget) {
    const std::string text_body =
        ReplaceAllLiteral(TrimSpaces(current_en), "--", kDoubleHorizontalBar);
    std::vector<NpsTextPair> ruby_list = meta.ruby_list;

    std::string protected_text = text_body;
    for (const auto& ruby : ruby_list) {
        if (!ruby.first.empty() && ruby.first.find(' ') != std::string::npos &&
            protected_text.find(ruby.first) != std::string::npos) {
            const std::string nbsp_phrase =
                ReplaceAllLiteral(ruby.first, " ", kNoBreakSpaceUtf8);
            protected_text = ReplaceFirstLiteral(protected_text, ruby.first, nbsp_phrase);
        }
    }

    std::vector<std::string> words;
    std::size_t start = 0;
    while (start <= protected_text.size()) {
        const std::size_t space_pos = protected_text.find(' ', start);
        const std::string_view token =
            (space_pos == std::string::npos)
                ? std::string_view(protected_text).substr(start)
                : std::string_view(protected_text).substr(start, space_pos - start);
        if (!token.empty()) {
            words.push_back(ReplaceAllLiteral(token, kNoBreakSpaceUtf8, kFullwidthSpace));
        }
        if (space_pos == std::string::npos) {
            break;
        }
        start = space_pos + 1;
    }
    if (words.empty()) {
        return {};
    }

    const int line_budget = std::max(kMinTypewriterColumnBudget, budget - kFullwidthColumnWidth);
    std::vector<std::string> v_lines;
    std::vector<std::string> cur_words;
    int cur_vis = ComputeVisibleColumns(meta.first_indent);

    for (const auto& word : words) {
        const int w_vis = ComputeVisibleColumns(word);
        const int sep_vis = cur_words.empty() ? 0 : kFullwidthColumnWidth;
        if (!cur_words.empty() && (cur_vis + sep_vis + w_vis > line_budget)) {
            v_lines.push_back(JoinStrings(cur_words, kFullwidthSpace));
            cur_words = {word};
            cur_vis = kFullwidthColumnWidth + w_vis;
        } else {
            cur_words.push_back(word);
            cur_vis += sep_vis + w_vis;
        }
    }
    if (!cur_words.empty()) {
        v_lines.push_back(JoinStrings(cur_words, kFullwidthSpace));
    }

    std::vector<NpsTextPair> rem_ruby = ruby_list;
    std::vector<std::unordered_map<std::size_t, std::string>> line_ruby_starts;
    std::vector<std::set<std::size_t>> line_ruby_ends;
    line_ruby_starts.reserve(v_lines.size());
    line_ruby_ends.reserve(v_lines.size());

    for (const auto& v_line : v_lines) {
        const std::vector<std::string> line_cps = SplitUtf8Codepoints(v_line);
        std::unordered_map<std::size_t, std::string> starts;
        std::set<std::size_t> ends;
        std::vector<NpsTextPair> next_rem;
        for (const auto& ruby : rem_ruby) {
            const std::string target = ReplaceAllLiteral(ruby.first, " ", kFullwidthSpace);
            const std::vector<std::string> target_cps = SplitUtf8Codepoints(target);
            const auto pos = FindCodepointSubsequence(line_cps, target_cps);
            if (pos.has_value() && starts.count(*pos) == 0) {
                starts[*pos] = ruby.second;
                ends.insert(*pos + target_cps.size() - 1);
            } else {
                next_rem.push_back(ruby);
            }
        }
        rem_ruby = std::move(next_rem);
        line_ruby_starts.push_back(std::move(starts));
        line_ruby_ends.push_back(std::move(ends));
    }

    std::string out_str;
    bool emitted_any = false;
    std::string pending_prefix = meta.first_indent;

    for (std::size_t l_idx = 0; l_idx < v_lines.size(); ++l_idx) {
        const std::vector<std::string> line_cps = SplitUtf8Codepoints(v_lines[l_idx]);
        const auto& starts = line_ruby_starts[l_idx];
        const auto& ends = line_ruby_ends[l_idx];
        bool first_char_on_line = true;

        for (std::size_t c_idx = 0; c_idx < line_cps.size(); ++c_idx) {
            const auto start_it = starts.find(c_idx);
            if (start_it != starts.end()) {
                pending_prefix += "<r text=\"" + start_it->second + "\">";
            }
            const std::string& ch = line_cps[c_idx];
            if (ch == kFullwidthSpace) {
                pending_prefix.append(kFullwidthSpace);
                if (ends.count(c_idx) != 0) {
                    pending_prefix += "</r>";
                }
                continue;
            }

            if (emitted_any) {
                if (first_char_on_line && l_idx > 0) {
                    out_str += "\r";
                    out_str.append(kTypewriterKeystrokeSeTag);
                    out_str.append(kFullwidthSpace);
                    out_str += "\r";
                    out_str.append(kFullwidthSpace);
                } else {
                    out_str += "\r";
                    out_str.append(kTypewriterKeystrokeSeTag);
                    out_str += "\r";
                }
            }

            out_str += pending_prefix;
            out_str += ch;
            if (ends.count(c_idx) != 0) {
                out_str += "</r>";
            }
            out_str += R"(<WAIT TIME="2">)";
            pending_prefix.clear();
            emitted_any = true;
            first_char_on_line = false;
        }
    }

    if (!pending_prefix.empty()) {
        out_str += pending_prefix;
    }

    boost::cmatch trail_se_match;
    if (boost::regex_search(meta.orig_raw_en.c_str(), trail_se_match,
                            TrailingTypewriterSeRegex())) {
        out_str.append(trail_se_match[1].first, trail_se_match[1].second);
    } else if (meta.trailing_k) {
        out_str += "<k>";
    }
    return out_str;
}

static std::size_t CountRegexMatches(std::string_view text, const boost::regex& pattern) {
    std::size_t count = 0;
    const char* cur = text.data();
    const char* end = text.data() + text.size();
    boost::cmatch match;
    while (boost::regex_search(cur, end, match, pattern)) {
        ++count;
        cur = match[0].second;
    }
    return count;
}

static std::pair<std::string, std::string> DetectBoundaryFontTags(const NpsBlockMetadata& meta) {
    if (!meta.boundary_prefix_tag.empty() || !meta.boundary_suffix_tag.empty()) {
        return {meta.boundary_prefix_tag, meta.boundary_suffix_tag};
    }

    std::string prefix_tag;
    std::string suffix_tag;
    const std::size_t opens = CountRegexMatches(meta.orig_raw_en, FontOpenRegex());
    const std::size_t closes = CountRegexMatches(meta.orig_raw_en, FontCloseRegex());
    if (opens > closes) {
        boost::cmatch match;
        if (boost::regex_search(meta.orig_raw_en.c_str(), match, BoundaryPrefixFontRegex())) {
            prefix_tag.assign(match[1].first, match[1].second);
        }
    } else if (closes > opens) {
        boost::cmatch match;
        if (boost::regex_search(meta.orig_raw_en.c_str(), match, BoundarySuffixFontRegex())) {
            suffix_tag.assign(match[1].first, match[1].second);
        } else {
            suffix_tag = "</FONT>";
        }
    }
    return {std::move(prefix_tag), std::move(suffix_tag)};
}

static std::vector<std::string> SplitOnCarriageReturn(std::string_view text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t pos = text.find('\r', start);
        if (pos == std::string_view::npos) {
            lines.emplace_back(text.substr(start));
            break;
        }
        lines.emplace_back(text.substr(start, pos - start));
        start = pos + 1;
    }
    return lines;
}

static boost::json::value ParseJsonFileWithRetry(const std::string& path) {
    for (int attempt = 0; attempt < kJsonReadMaxAttempts; ++attempt) {
        try {
            const Bytes raw = read_file(path);
            if (!raw.empty()) {
                return boost::json::parse(boost::json::string_view(
                    reinterpret_cast<const char*>(raw.data()), raw.size()));
            }
        } catch (const std::exception&) {
            if (attempt + 1 == kJsonReadMaxAttempts) {
                throw;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kJsonReadRetryDelayMs));
    }
    throw std::runtime_error("failed to read non-empty JSON file: " + path);
}

static std::vector<NpsTextPair> ParsePairArray(const boost::json::array& arr) {
    std::vector<NpsTextPair> out;
    out.reserve(arr.size());
    for (const auto& elem : arr) {
        const auto& pair_arr = elem.as_array();
        NpsTextPair pair;
        pair.first = std::string(pair_arr.at(0).as_string());
        pair.second = std::string(pair_arr.at(1).as_string());
        out.push_back(std::move(pair));
    }
    return out;
}

static NpsBlockMetadata ParseBlockMetadata(const boost::json::object& obj) {
    NpsBlockMetadata meta;
    meta.box_type = static_cast<int>(obj.at("box_type").to_number<int>());
    meta.first_indent = std::string(obj.at("first_indent").as_string());
    meta.trailing_k = obj.at("trailing_k").as_bool();
    meta.k_clause_count = static_cast<int>(obj.at("k_clause_count").to_number<int>());

    const auto& clauses_arr = obj.at("orig_k_clauses_clean").as_array();
    meta.orig_k_clauses_clean.reserve(clauses_arr.size());
    for (const auto& item : clauses_arr) {
        meta.orig_k_clauses_clean.emplace_back(item.as_string());
    }

    meta.ruby_list = ParsePairArray(obj.at("ruby_list").as_array());
    meta.font_list = ParsePairArray(obj.at("font_list").as_array());
    meta.wait_list = ParsePairArray(obj.at("wait_list").as_array());
    meta.is_typewriter = obj.at("is_typewriter").as_bool();
    meta.has_complex_fx = obj.at("has_complex_fx").as_bool();
    meta.orig_clean_en = std::string(obj.at("orig_clean_en").as_string());
    meta.orig_raw_en = std::string(obj.at("orig_raw_en").as_string());
    if (const auto* p = obj.if_contains("boundary_prefix_tag")) {
        meta.boundary_prefix_tag = std::string(p->as_string());
    }
    if (const auto* s = obj.if_contains("boundary_suffix_tag")) {
        meta.boundary_suffix_tag = std::string(s->as_string());
    }
    return meta;
}

}  // namespace

BoxWidthPolicy::~BoxWidthPolicy() = default;

DefaultBoxWidthPolicy::~DefaultBoxWidthPolicy() = default;

int DefaultBoxWidthPolicy::ColumnBudgetForBox(int box_type) const noexcept {
    switch (box_type) {
    case kBoxType1:
    case kBoxType2:
    case kBoxType3:
    case kBoxType4:
        return kNarrowBoxColumnBudget;
    case kBoxType5:
    case kBoxType6:
    case kBoxType7:
    case kBoxType8:
    case kBoxType10:
        return kMediumBoxColumnBudget;
    case kBoxType0:
    case kBoxType9:
    case kBoxType11:
    case kBoxType12:
    case kBoxType13:
    default:
        return kWideBoxColumnBudget;
    }
}

bool DefaultBoxWidthPolicy::IsFourLinePaginatedBox(int box_type) const noexcept {
    return box_type >= kBoxType0 && box_type <= kMaxFourLinePaginatedBoxType;
}

int ComputeVisibleColumns(std::string_view utf8_text) {
    const std::string ruby_unwrapped = boost::regex_replace(
        std::string(utf8_text), RubyWholeRegex(), "$1");
    const std::string tag_stripped =
        boost::regex_replace(ruby_unwrapped, SingleTagRegex(), "");

    int columns = 0;
    std::size_t pos = 0;
    while (pos < tag_stripped.size()) {
        const char32_t cp = DecodeNextUtf8Codepoint(tag_stripped, pos);
        columns += (cp > kMaxAsciiCodepoint) ? kFullwidthColumnWidth : kHalfwidthColumnWidth;
    }
    return columns;
}

std::vector<std::string> TokenizeForWrap(std::string_view annotated_clause) {
    std::vector<std::string> tokens;
    std::string cur;
    const char* begin = annotated_clause.data();
    const char* end = annotated_clause.data() + annotated_clause.size();
    std::size_t i = 0;

    while (i < annotated_clause.size()) {
        boost::cmatch ruby_match;
        if (boost::regex_search(begin + i, end, ruby_match, RubyWholeRegex(),
                                boost::match_continuous)) {
            cur.append(ruby_match[0].first, ruby_match[0].second);
            i += static_cast<std::size_t>(ruby_match[0].length());
            continue;
        }
        boost::cmatch tag_match;
        if (boost::regex_search(begin + i, end, tag_match, SingleTagRegex(),
                                boost::match_continuous)) {
            cur.append(tag_match[0].first, tag_match[0].second);
            i += static_cast<std::size_t>(tag_match[0].length());
            continue;
        }
        if (annotated_clause[i] == ' ') {
            if (!cur.empty()) {
                tokens.push_back(std::move(cur));
                cur.clear();
            }
            ++i;
            continue;
        }
        cur.push_back(annotated_clause[i]);
        ++i;
    }

    if (!cur.empty()) {
        tokens.push_back(std::move(cur));
    }
    return tokens;
}

std::string ApplyInlineAnnotations(std::string_view clause,
                                   std::vector<NpsTextPair>& ruby_list,
                                   std::vector<NpsTextPair>& font_list,
                                   std::vector<NpsTextPair>& wait_list) {
    std::string s(clause);

    std::vector<NpsTextPair> rem_ruby;
    for (const auto& ruby : ruby_list) {
        if (ruby.first.empty()) {
            continue;
        }
        std::vector<std::string> parts = SplitKeepingDelimiters(s, TagOrElemSplitRegex());
        bool matched = false;
        for (std::size_t idx = 0; idx < parts.size(); idx += kEvenSegmentStep) {
            const std::size_t pos = parts[idx].find(ruby.first);
            if (pos != std::string::npos) {
                const std::string replacement =
                    "<r text=\"" + ruby.second + "\">" + ruby.first + "</r>";
                parts[idx].replace(pos, ruby.first.size(), replacement);
                matched = true;
                break;
            }
        }
        if (matched) {
            s = JoinStrings(parts, "");
        } else {
            rem_ruby.push_back(ruby);
        }
    }
    ruby_list = std::move(rem_ruby);

    std::vector<NpsTextPair> rem_font;
    for (const auto& font : font_list) {
        if (font.first.empty()) {
            continue;
        }
        std::vector<std::string> parts = SplitKeepingDelimiters(s, FontSplitRegex());
        bool matched = false;
        const std::string close_tag =
            EndsWithIgnoreCase(font.second, "<I>") ? "</I></FONT>" : "</FONT>";
        for (std::size_t idx = 0; idx < parts.size(); idx += kEvenSegmentStep) {
            const std::size_t pos = parts[idx].find(font.first);
            if (pos != std::string::npos) {
                const std::string replacement = font.second + font.first + close_tag;
                parts[idx].replace(pos, font.first.size(), replacement);
                matched = true;
                break;
            }
        }
        if (matched) {
            s = JoinStrings(parts, "");
        } else {
            rem_font.push_back(font);
        }
    }
    font_list = std::move(rem_font);

    std::vector<NpsTextPair> rem_wait;
    std::optional<std::string> last_tail_key;
    std::size_t last_plain_end = 0;

    for (const auto& wait : wait_list) {
        if (wait.first.empty()) {
            boost::cmatch lead_match;
            std::size_t ins_at = 0;
            if (boost::regex_search(s.c_str(), lead_match, LeadingWaitRegex())) {
                ins_at = static_cast<std::size_t>(lead_match[0].length());
            }
            s.insert(ins_at, wait.second);
            last_tail_key = "";
            last_plain_end = 0;
            continue;
        }

        const auto [s_plain, plain_to_s_end] = StripTagsWithMap(s);
        std::string search_key = wait.first;
        std::size_t pos = std::string::npos;

        if (last_tail_key.has_value() && search_key == *last_tail_key) {
            const std::size_t next_start =
                (last_plain_end + 1 > search_key.size())
                    ? (last_plain_end + 1 - search_key.size())
                    : 0;
            pos = s_plain.find(search_key, next_start);
        }
        if (pos == std::string::npos) {
            pos = s_plain.find(search_key);
        }
        if (pos == std::string::npos) {
            const std::string short_key =
                boost::regex_replace(search_key, SentencePrefixRegex(), "");
            if (!short_key.empty() && short_key != search_key) {
                search_key = short_key;
                pos = s_plain.find(search_key);
            }
        }

        if (pos != std::string::npos) {
            const std::size_t end_plain_idx = pos + search_key.size() - 1;
            const std::size_t ins_idx = plain_to_s_end[end_plain_idx];
            s.insert(ins_idx, wait.second);
            last_tail_key = wait.first;
            last_plain_end = end_plain_idx + 1;
        } else {
            rem_wait.push_back(wait);
        }
    }
    wait_list = std::move(rem_wait);

    std::vector<std::string> parts = SplitKeepingDelimiters(s, AnyTagSplitRegex());
    for (std::size_t idx = 0; idx < parts.size(); idx += kEvenSegmentStep) {
        parts[idx] = ReplaceHyphenRunsWithHorizontalBars(parts[idx]);
    }
    return JoinStrings(parts, "");
}

std::vector<std::string> WrapAnnotatedClause(std::string_view annotated_clause,
                                             int budget,
                                             std::string_view first_indent,
                                             std::string_view cont_indent,
                                             bool attach_k) {
    const std::vector<std::string> tokens = TokenizeForWrap(annotated_clause);
    if (tokens.empty()) {
        return {};
    }

    std::vector<std::string> lines;
    std::string cur_indent(first_indent);
    std::vector<std::string> cur_tokens;
    int cur_vis = ComputeVisibleColumns(cur_indent);

    for (const auto& tok : tokens) {
        const int t_vis = ComputeVisibleColumns(tok);
        const int space_vis = cur_tokens.empty() ? 0 : kHalfwidthColumnWidth;
        if (!cur_tokens.empty() && (cur_vis + space_vis + t_vis > budget)) {
            lines.push_back(cur_indent + JoinStrings(cur_tokens, " "));
            cur_indent = std::string(cont_indent);
            cur_tokens = {tok};
            cur_vis = ComputeVisibleColumns(cur_indent) + t_vis;
        } else {
            cur_tokens.push_back(tok);
            cur_vis += space_vis + t_vis;
        }
    }

    if (!cur_tokens.empty()) {
        lines.push_back(cur_indent + JoinStrings(cur_tokens, " "));
    }

    if (attach_k && !lines.empty()) {
        if (!boost::regex_search(lines.back(), TrailingKRegex())) {
            lines.back() += "<k>";
        }
    }
    return lines;
}

struct NpsRebuilder::Impl {
    std::shared_ptr<const BoxWidthPolicy> width_policy;
};

NpsRebuilder::NpsRebuilder()
    : NpsRebuilder(std::make_shared<DefaultBoxWidthPolicy>()) {}

NpsRebuilder::NpsRebuilder(std::shared_ptr<const BoxWidthPolicy> width_policy)
    : impl_(std::make_unique<Impl>()) {
    if (!width_policy) {
        throw std::invalid_argument("NpsRebuilder requires a non-null BoxWidthPolicy");
    }
    impl_->width_policy = std::move(width_policy);
}

NpsRebuilder::NpsRebuilder(const NpsRebuilder& other)
    : impl_(std::make_unique<Impl>(*other.impl_)) {}

NpsRebuilder::NpsRebuilder(NpsRebuilder&& other) noexcept = default;

NpsRebuilder& NpsRebuilder::operator=(const NpsRebuilder& other) {
    if (this != &other) {
        *impl_ = *other.impl_;
    }
    return *this;
}

NpsRebuilder& NpsRebuilder::operator=(NpsRebuilder&& other) noexcept = default;

NpsRebuilder::~NpsRebuilder() = default;

const BoxWidthPolicy& NpsRebuilder::WidthPolicy() const noexcept {
    return *impl_->width_policy;
}

NpsBlockResult NpsRebuilder::ReconstructBlock(std::string_view current_en,
                                              const NpsBlockMetadata& meta) const {
    const int budget = impl_->width_policy->ColumnBudgetForBox(meta.box_type);
    const bool modified = (current_en != meta.orig_clean_en);

    if (meta.is_typewriter) {
        if (!modified) {
            return {meta.orig_raw_en, false, false};
        }
        return {ReconstructTypewriter(current_en, meta, budget), true, true};
    }

    const std::size_t first_indent_cps = CountUtf8Codepoints(meta.first_indent);
    const bool is_centered = (first_indent_cps >= kCenteredIndentMinCodepoints) &&
                             (meta.first_indent.find(kFullwidthSpace) != std::string::npos);

    if (!modified && (meta.has_complex_fx || is_centered)) {
        const std::vector<std::string> raw_lines = SplitOnCarriageReturn(meta.orig_raw_en);
        int max_vis = 0;
        for (const auto& line : raw_lines) {
            max_vis = std::max(max_vis, ComputeVisibleColumns(line));
        }
        if (meta.has_complex_fx || is_centered || max_vis <= budget) {
            return {meta.orig_raw_en, false, false};
        }
    }

    if (!modified) {
        const std::vector<std::string> raw_lines = SplitOnCarriageReturn(meta.orig_raw_en);
        const bool all_fit = std::all_of(raw_lines.begin(), raw_lines.end(),
                                         [budget](const std::string& line) {
                                             return ComputeVisibleColumns(line) <= budget;
                                         });
        if (all_fit) {
            return {meta.orig_raw_en, false, false};
        }
    }

    std::vector<std::string> clauses;
    if (!modified && meta.orig_k_clauses_clean.size() > 1) {
        clauses = meta.orig_k_clauses_clean;
    } else if (modified && meta.k_clause_count > 1) {
        clauses = SplitModifiedIntoClauses(current_en, meta.k_clause_count);
    } else {
        clauses = {std::string(current_en)};
    }

    std::vector<NpsTextPair> ruby_list = meta.ruby_list;
    std::vector<NpsTextPair> font_list = meta.font_list;
    std::vector<NpsTextPair> wait_list = meta.wait_list;

    const std::string c0_first_indent =
        (first_indent_cps <= kMaxPreservedFirstIndentCodepoints)
            ? meta.first_indent
            : std::string(kFullwidthSpace);
    const std::string cont_indent(kFullwidthSpace);

    std::vector<std::vector<std::string>> clause_lines_list;
    for (std::size_t c_idx = 0; c_idx < clauses.size(); ++c_idx) {
        const std::string annotated =
            ApplyInlineAnnotations(clauses[c_idx], ruby_list, font_list, wait_list);
        const bool attach_k = (c_idx + 1 < clauses.size()) || meta.trailing_k;
        const std::string& f_ind = (c_idx == 0) ? c0_first_indent : cont_indent;
        std::vector<std::string> wrapped_lines =
            WrapAnnotatedClause(annotated, budget, f_ind, cont_indent, attach_k);
        if (!wrapped_lines.empty()) {
            clause_lines_list.push_back(std::move(wrapped_lines));
        }
    }

    std::string rebuilt;
    if (impl_->width_policy->IsFourLinePaginatedBox(meta.box_type)) {
        std::vector<std::string> pages;
        std::vector<std::string> cur_page;
        for (const auto& wrapped_lines : clause_lines_list) {
            if (!cur_page.empty() &&
                cur_page.size() + wrapped_lines.size() > kMaxLinesPerFourLineBox) {
                if (!boost::regex_search(cur_page.back(), TrailingKRegex())) {
                    cur_page.back() += "<k>";
                }
                pages.push_back(JoinStrings(cur_page, "\r"));
                cur_page.clear();
            }
            for (const auto& line : wrapped_lines) {
                cur_page.push_back(line);
                if (cur_page.size() == kMaxLinesPerFourLineBox) {
                    if (!boost::regex_search(cur_page.back(), TrailingKRegex())) {
                        cur_page.back() += "<k>";
                    }
                    pages.push_back(JoinStrings(cur_page, "\r"));
                    cur_page.clear();
                }
            }
        }
        if (!cur_page.empty()) {
            pages.push_back(JoinStrings(cur_page, "\r"));
        }
        rebuilt = JoinStrings(pages, "\r\r");
    } else {
        std::vector<std::string> flat_lines;
        for (const auto& wrapped_lines : clause_lines_list) {
            for (const auto& line : wrapped_lines) {
                flat_lines.push_back(line);
            }
        }
        rebuilt = JoinStrings(flat_lines, "\r");
    }

    const auto [prefix_tag, suffix_tag] = DetectBoundaryFontTags(meta);
    if (!prefix_tag.empty() &&
        CountRegexMatches(rebuilt, FontOpenRegex()) <
            CountRegexMatches(meta.orig_raw_en, FontOpenRegex())) {
        boost::cmatch indent_match;
        std::size_t indent_len = 0;
        if (boost::regex_search(rebuilt.c_str(), indent_match, LeadingIndentRegex())) {
            indent_len = static_cast<std::size_t>(indent_match[0].length());
        }
        rebuilt.insert(indent_len, prefix_tag);
    }
    if (!suffix_tag.empty() &&
        CountRegexMatches(rebuilt, FontCloseRegex()) <
            CountRegexMatches(meta.orig_raw_en, FontCloseRegex())) {
        boost::cmatch mid_wait_match;
        if (boost::regex_search(rebuilt.c_str(), mid_wait_match, MidWaitTime5Regex()) &&
            boost::regex_search(mid_wait_match[0].second, rebuilt.c_str() + rebuilt.size(),
                                FontOpenRegex())) {
            const auto insert_pos =
                static_cast<std::size_t>(mid_wait_match[0].first - rebuilt.c_str());
            rebuilt.insert(insert_pos, suffix_tag);
        } else {
            rebuilt += suffix_tag;
        }
    }

    return {std::move(rebuilt), modified, true};
}

std::vector<PakEntry> NpsRebuilder::BuildEntriesFromJsonFiles(
    const std::string& translated_json_path,
    const std::string& recon_map_json_path,
    NpsRebuildStats* out_stats) const {
    const boost::json::value translated_val = ParseJsonFileWithRetry(translated_json_path);
    const boost::json::value recon_val = ParseJsonFileWithRetry(recon_map_json_path);

    const auto& translated_root = translated_val.as_object();
    const auto& recon_root = recon_val.as_object();

    NpsRebuildStats stats{};
    std::vector<PakEntry> entries;
    entries.reserve(recon_root.size() + 4);

    for (const auto& kv : recon_root) {
        const std::string fn(kv.key());
        if (fn == kNonNpsFilesKey) {
            continue;
        }
        ++stats.total_nps_scripts;

        const auto& script_recon = kv.value().as_object();
        const auto& chunks_arr = script_recon.at("chunks").as_array();
        const auto& blocks_arr = script_recon.at("blocks").as_array();

        std::string rebuilt_nps;
        if (blocks_arr.empty()) {
            rebuilt_nps = std::string(chunks_arr.at(0).as_string());
        } else {
            const auto& trn_entries =
                translated_root.at(fn).as_object().at("strings").as_array();
            if (trn_entries.size() != blocks_arr.size()) {
                throw std::runtime_error("entry count mismatch in " + fn);
            }

            rebuilt_nps = std::string(chunks_arr.at(0).as_string());
            for (std::size_t i = 0; i < blocks_arr.size(); ++i) {
                const auto& entry_obj = trn_entries.at(i).as_object();
                const std::string_view cur_en = entry_obj.at("translated").as_string();
                const NpsBlockMetadata meta = ParseBlockMetadata(blocks_arr.at(i).as_object());

                const NpsBlockResult res = ReconstructBlock(cur_en, meta);
                ++stats.total_blocks;
                if (res.was_modified) {
                    ++stats.modified_blocks;
                }
                if (res.was_rewrapped) {
                    ++stats.rewrapped_blocks;
                }
                rebuilt_nps += res.text;
                rebuilt_nps.append(chunks_arr.at(i + 1).as_string());
            }
        }

        rebuilt_nps = boost::regex_replace(rebuilt_nps, BoxType13Regex(), R"(<BOX type="12">)");

        PakEntry pak_entry;
        pak_entry.relative_path = "script\\" + fn;
        pak_entry.payload = Utf8ToCp932(rebuilt_nps);
        pak_entry.compress = true;
        entries.push_back(std::move(pak_entry));
    }

    if (const auto* non_nps = recon_root.if_contains(kNonNpsFilesKey)) {
        for (const auto& kv : non_nps->as_object()) {
            const std::string fn(kv.key());
            const std::string_view content = kv.value().as_string();
            PakEntry pak_entry;
            pak_entry.relative_path = "script\\" + fn;
            pak_entry.payload = Utf8ToCp932(content);
            pak_entry.compress = true;
            entries.push_back(std::move(pak_entry));
        }
    }

    stats.total_pak_files = entries.size();
    if (out_stats != nullptr) {
        *out_stats = stats;
    }
    return entries;
}

NpsRebuildStats NpsRebuilder::RebuildAndDeploy(
    const std::string& project_dir,
    const std::vector<std::string>& output_game_dirs,
    bool verify_archive) const {
    const std::string script_output_dir = patch::path_join(project_dir, "script_output");
    const std::string translated_json =
        patch::path_join(script_output_dir, "translated_text.json");
    const std::string recon_map_json =
        patch::path_join(script_output_dir, "nps_reconstruction_map.json");

    NpsRebuildStats stats{};
    const std::vector<PakEntry> entries =
        BuildEntriesFromJsonFiles(translated_json, recon_map_json, &stats);

    const Bytes pak_bytes = WritePakArchive(entries, kDefaultPakMagicName);
    stats.pak_size_bytes = pak_bytes.size();

    if (verify_archive) {
        VerifyPakArchive(pak_bytes, entries);
    }

    for (const auto& game_dir : output_game_dirs) {
        if (game_dir.empty()) {
            continue;
        }
        const std::string script_dir = patch::path_join(game_dir, "script");
        std::error_code ec;
        fs::create_directories(fs::u8path(script_dir), ec);
        if (ec) {
            throw std::runtime_error("cannot create directory " + script_dir + ": " +
                                     ec.message());
        }

        for (const auto& entry : entries) {
            const std::string base_fn = patch::base_name(entry.relative_path);
            const std::string out_path = patch::path_join(script_dir, base_fn);
            write_file(out_path, entry.payload);
        }

        const std::string pak_path = patch::path_join(game_dir, "0script.pak");
        write_file(pak_path, pak_bytes);
    }

    return stats;
}

}  // namespace ksh::pak
