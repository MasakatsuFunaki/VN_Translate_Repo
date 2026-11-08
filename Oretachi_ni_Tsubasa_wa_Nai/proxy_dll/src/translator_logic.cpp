// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include "translator_logic.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <istream>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace oretsuba::proxy {

namespace {

constexpr UINT kCodePageShiftJis = 932;
constexpr char kMessageHeaderPrefix = '#';
constexpr char kMessageDelimiterChar = '$';
constexpr char kMessageAmpersandChar = '&';
constexpr char kLineBreakChar = '\\';
constexpr std::string_view kMidlinePauseToken = "$&";
constexpr std::string_view kAngleRubyOpenTag = "<\"";
constexpr std::string_view kAngleRubyCloseTag = "\">";
constexpr std::size_t kRubyOpenTagLen = kAngleRubyOpenTag.size();
constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";
constexpr std::string_view kUtf8FullwidthSpace = "\xE3\x80\x80";
constexpr std::string_view kCp932FullwidthSpace = "\x81\x40";

constexpr std::uint8_t kAsciiUpperBound = 0x80;
constexpr std::uint8_t kUtf8MinTwoByteLead = 0xC2;
constexpr std::uint8_t kSjisLeadRange1Min = 0x81;
constexpr std::uint8_t kSjisLeadRange1Max = 0x9F;
constexpr std::uint8_t kSjisLeadRange2Min = 0xE0;
constexpr std::uint8_t kSjisLeadRange2Max = 0xFC;
constexpr std::uint8_t kUtf8ContMask = 0xC0;
constexpr std::uint8_t kUtf8ContValue = 0x80;
constexpr std::uint8_t kUtf8TwoByteMask = 0xE0;
constexpr std::uint8_t kUtf8TwoByteValue = 0xC0;
constexpr std::uint8_t kUtf8ThreeByteMask = 0xF0;
constexpr std::uint8_t kUtf8ThreeByteValue = 0xE0;
constexpr std::uint8_t kUtf8FourByteMask = 0xF8;
constexpr std::uint8_t kUtf8FourByteValue = 0xF0;
constexpr std::size_t kCp932DoubleByteLen = 2;
constexpr std::size_t kUtf8TwoByteLen = 2;
constexpr std::size_t kUtf8ThreeByteLen = 3;
constexpr std::size_t kUtf8FourByteLen = 4;
constexpr std::size_t kMaxBraceControlTagLen = 16;
constexpr std::size_t kWrapReserveSlackBytes = 4;

struct SpeakerGlossaryPair {
    const char* jp_utf8;
    const char* en_name;
};

constexpr SpeakerGlossaryPair kBuiltinSpeakers[] = {
    {"タカシ", "Takashi"},
    {"鷹志", "Takashi"},
    {"羽田鷹志", "Takashi Haneda"},
    {"羽田", "Haneda"},
    {"小鳩", "Kobato"},
    {"羽田小鳩", "Kobato Haneda"},
    {"明日香", "Asuka"},
    {"渡来明日香", "Asuka Watarai"},
    {"渡来", "Watarai"},
    {"日和子", "Hiyoko"},
    {"玉泉日和子", "Hiyoko Tamaizumi"},
    {"玉泉", "Tamaizumi"},
    {"鳴", "Naru"},
    {"鳳鳴", "Naru Ootori"},
    {"鳳", "Ootori"},
    {"隼人", "Hayato"},
    {"成田隼人", "Hayato Narita"},
    {"成田", "Narita"},
    {"鷲介", "Shuusuke"},
    {"千歳鷲介", "Shuusuke Chitose"},
    {"京", "Kyou"},
    {"山科京", "Kyou Yamashina"},
    {"山科", "Yamashina"},
    {"愛", "Ai"},
    {"香田愛", "Ai Kouda"},
    {"亜衣", "Ai"},
    {"香田亜衣", "Ai Kohda"},
    {"香田", "Kouda"},
    {"美空", "Misora"},
    {"春日春恵", "Harue Kasuga"},
    {"春恵", "Harue"},
    {"春日", "Kasuga"},
    {"千歳", "Chitose"},
    {"望月紀奈子", "Kinako Mochizuki"},
    {"紀奈子", "Kinako"},
    {"望月", "Mochizuki"},
    {"日野英里子", "Eriko Hino"},
    {"英里子", "Eriko"},
    {"日野", "Hino"},
    {"米田優", "Yuu Yoneda"},
    {"米田", "Yoneda"},
    {"優", "Yuu"},
    {"針生蔵人", "Kuroudo Hariu"},
    {"針生", "Hariu"},
    {"蔵人", "Kuroudo"},
    {"翔", "Kakeru"},
    {"鳳翔", "Kakeru Ootori"},
    {"カケル", "Kakeru"},
    {"ヨウ", "You"},
    {"ヨージ", "Yoji"},
    {"森里和馬", "Kazuma Morisato"},
    {"森里", "Morisato"},
    {"和馬", "Kazuma"},
    {"軽部狩男", "Kario Karube"},
    {"軽部", "Karube"},
    {"狩男", "Kario"},
    {"ガルーダ", "Garuda"},
    {"聖騎士ホーク", "Holy Knight Hawk"},
    {"ホーク", "Hawk"},
    {"王女アスカ", "Princess Asuka"},
    {"騎士ペガサス", "Knight Pegasus"},
    {"ペガサス", "Pegasus"},
    {"伽楼羅", "Karura"},
    {"咲夜", "Sakuya"},
    {"アリス", "Alice"},
    {"プラチナ", "Platina"},
    {"マルチネス", "Martinez"},
    {"ガロウ", "Garou"},
    {"バニィD", "Bunny D"},
    {"バニィＤ", "Bunny D"},
    {"チサト", "Chisato"},
    {"千里", "Chisato"},
    {"高内昌子", "Masako Takauchi"},
    {"昌子", "Masako"},
    {"高内", "Takauchi"},
    {"土門", "Domon"},
    {"リンダ", "Linda"},
    {"少年", "Boy"},
    {"少女", "Girl"},
    {"男子生徒", "Male Student"},
    {"女子生徒", "Female Student"},
    {"教師", "Teacher"},
    {"店員", "Clerk"},
    {"客", "Customer"},
    {"声", "Voice"},
    {"？？", "??"},
    {"？？？", "???"},
};

bool IsSjisLeadByte(std::uint8_t b) noexcept {
    return (b >= kSjisLeadRange1Min && b <= kSjisLeadRange1Max) ||
           (b >= kSjisLeadRange2Min && b <= kSjisLeadRange2Max);
}

std::size_t DetermineUtf8SequenceLength(std::uint8_t lead_byte) noexcept {
    if ((lead_byte & kUtf8TwoByteMask) == kUtf8TwoByteValue) {
        return (lead_byte >= kUtf8MinTwoByteLead) ? kUtf8TwoByteLen : 0U;
    }
    if ((lead_byte & kUtf8ThreeByteMask) == kUtf8ThreeByteValue) {
        return kUtf8ThreeByteLen;
    }
    if ((lead_byte & kUtf8FourByteMask) == kUtf8FourByteValue) {
        return kUtf8FourByteLen;
    }
    return 0U;
}

bool IsValidUtf8(std::string_view s) noexcept {
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<std::uint8_t>(s[i]);
        if (c < kAsciiUpperBound) {
            ++i;
            continue;
        }
        const std::size_t seq_len = DetermineUtf8SequenceLength(c);
        if (seq_len == 0U || i + seq_len > s.size()) {
            return false;
        }
        for (std::size_t j = 1; j < seq_len; ++j) {
            const auto cont = static_cast<std::uint8_t>(s[i + j]);
            if ((cont & kUtf8ContMask) != kUtf8ContValue) {
                return false;
            }
        }
        i += seq_len;
    }
    return true;
}

std::size_t FindStandaloneAsciiBackslash(std::string_view text, bool is_utf8) noexcept {
    if (is_utf8) {
        return text.find(kLineBreakChar);
    }
    std::size_t i = 0;
    while (i < text.size()) {
        if (i + 1U < text.size() &&
            IsSjisLeadByte(static_cast<std::uint8_t>(text[i]))) {
            i += kCp932DoubleByteLen;
            continue;
        }
        if (text[i] == kLineBreakChar) {
            return i;
        }
        ++i;
    }
    return std::string_view::npos;
}

bool IsAsciiWhitespace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string TrimLeadingAndTrailingSpaces(std::string_view input) {
    std::size_t start = 0;
    while (start < input.size()) {
        if (IsAsciiWhitespace(input[start])) {
            ++start;
            continue;
        }
        if (input.substr(start).rfind(kUtf8FullwidthSpace, 0) == 0) {
            start += kUtf8FullwidthSpace.size();
            continue;
        }
        if (input.substr(start).rfind(kCp932FullwidthSpace, 0) == 0) {
            start += kCp932FullwidthSpace.size();
            continue;
        }
        break;
    }

    std::size_t end = input.size();
    while (end > start) {
        if (IsAsciiWhitespace(input[end - 1])) {
            --end;
            continue;
        }
        if (end >= start + kUtf8FullwidthSpace.size() &&
            input.substr(end - kUtf8FullwidthSpace.size(), kUtf8FullwidthSpace.size()) ==
                kUtf8FullwidthSpace) {
            end -= kUtf8FullwidthSpace.size();
            continue;
        }
        if (end >= start + kCp932FullwidthSpace.size() &&
            input.substr(end - kCp932FullwidthSpace.size(), kCp932FullwidthSpace.size()) ==
                kCp932FullwidthSpace) {
            end -= kCp932FullwidthSpace.size();
            continue;
        }
        break;
    }

    return std::string(input.substr(start, end - start));
}

std::size_t MeasureLeadingControlPrefix(std::string_view text) noexcept {
    std::size_t pos = 0;
    while (pos < text.size()) {
        if (text[pos] == '{') {
            const std::size_t close_brace = text.find('}', pos + 1U);
            if (close_brace != std::string_view::npos &&
                close_brace - pos <= kMaxBraceControlTagLen) {
                pos = close_brace + 1U;
                continue;
            }
        }
        if (text[pos] == '<') {
            std::size_t scan = pos + 1U;
            if (scan < text.size() && (text[scan] == '+' || text[scan] == '-')) {
                ++scan;
            }
            const std::size_t digits_start = scan;
            while (scan < text.size() && text[scan] >= '0' && text[scan] <= '9') {
                ++scan;
            }
            if (scan > digits_start && scan < text.size() && text[scan] == '>') {
                pos = scan + 1U;
                if (pos < text.size() && text[pos] == kLineBreakChar) {
                    ++pos;
                }
                continue;
            }
        }
        if (text.substr(pos).rfind(kMidlinePauseToken, 0) == 0) {
            pos += kMidlinePauseToken.size();
            continue;
        }
        break;
    }
    return pos;
}

std::string StripRubyMarkup(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    const bool is_utf8 = IsValidUtf8(text);
    std::size_t i = 0;
    while (i < text.size()) {
        if (!is_utf8 && i + 1U < text.size() &&
            IsSjisLeadByte(static_cast<std::uint8_t>(text[i]))) {
            out.append(text.data() + i, kCp932DoubleByteLen);
            i += kCp932DoubleByteLen;
            continue;
        }
        if (i + kRubyOpenTagLen < text.size() &&
            text.substr(i, kRubyOpenTagLen) == kAngleRubyOpenTag) {
            const std::size_t base_start = i + kRubyOpenTagLen;
            const std::size_t pipe_pos = text.find('|', base_start);
            const std::size_t close_pos = text.find(kAngleRubyCloseTag, base_start);
            if (pipe_pos != std::string_view::npos &&
                close_pos != std::string_view::npos &&
                pipe_pos < close_pos) {
                out.append(text.substr(base_start, pipe_pos - base_start));
                i = close_pos + kAngleRubyCloseTag.size();
                continue;
            }
        }
        if (text[i] == '[') {
            const std::size_t base_start = i + 1U;
            const std::size_t pipe_pos = text.find('|', base_start);
            const std::size_t close_pos = text.find(']', base_start);
            if (pipe_pos != std::string_view::npos &&
                close_pos != std::string_view::npos &&
                pipe_pos < close_pos) {
                out.append(text.substr(pipe_pos + 1U, close_pos - (pipe_pos + 1U)));
                i = close_pos + 1U;
                continue;
            }
        }
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

std::string WrapSingleSegmentWords(std::string_view segment, std::size_t max_columns) {
    const std::string cleaned = TrimLeadingAndTrailingSpaces(segment);
    if (cleaned.empty() || max_columns == 0U || cleaned.size() <= max_columns) {
        return cleaned;
    }

    std::vector<std::string_view> words;
    std::size_t pos = 0;
    while (pos < cleaned.size()) {
        while (pos < cleaned.size() && cleaned[pos] == ' ') {
            ++pos;
        }
        if (pos >= cleaned.size()) {
            break;
        }
        const std::size_t next_space = cleaned.find(' ', pos);
        if (next_space == std::string::npos) {
            words.push_back(std::string_view(cleaned).substr(pos));
            break;
        }
        words.push_back(std::string_view(cleaned).substr(pos, next_space - pos));
        pos = next_space + 1U;
    }

    std::string out;
    out.reserve(cleaned.size() + cleaned.size() / max_columns + kWrapReserveSlackBytes);
    std::size_t line_len = 0;

    for (std::string_view word : words) {
        if (line_len > 0U && line_len + 1U + word.size() <= max_columns) {
            out.push_back(' ');
            out.append(word.data(), word.size());
            line_len += 1U + word.size();
            continue;
        }

        if (line_len > 0U) {
            out.push_back(kLineBreakChar);
            line_len = 0;
        }

        while (word.size() > max_columns) {
            out.append(word.data(), max_columns);
            word.remove_prefix(max_columns);
            if (!word.empty()) {
                out.push_back(kLineBreakChar);
            }
        }

        if (!word.empty()) {
            out.append(word.data(), word.size());
            line_len = word.size();
        }
    }

    return out;
}

std::string WrapBackslashClause(std::string_view clause, std::size_t max_columns) {
    if (clause.find(kLineBreakChar) == std::string_view::npos) {
        return WrapSingleSegmentWords(clause, max_columns);
    }

    std::string out;
    std::size_t pos = 0;
    bool first = true;
    while (pos <= clause.size()) {
        const std::size_t next_bs = clause.find(kLineBreakChar, pos);
        const std::string_view part =
            (next_bs == std::string_view::npos)
                ? clause.substr(pos)
                : clause.substr(pos, next_bs - pos);
        const std::string wrapped_part = WrapSingleSegmentWords(part, max_columns);
        if (!wrapped_part.empty()) {
            if (!first) {
                out.push_back(kLineBreakChar);
            }
            out += wrapped_part;
            first = false;
        }
        if (next_bs == std::string_view::npos) {
            break;
        }
        pos = next_bs + 1U;
    }
    return out;
}

}  // namespace

struct TranslationCatalog::Impl {
    std::size_t max_wrap_columns = kDefaultWrapColumns;
    std::size_t canonical_entry_count = 0;
    std::deque<std::string> persistent_pool;
    std::unordered_map<std::string, const std::string*> lookup_map;
    std::unordered_map<std::string, std::string> speaker_map;

    explicit Impl(std::size_t wrap_cols) : max_wrap_columns(wrap_cols) {
        SeedBuiltinSpeakers();
    }

    void SeedBuiltinSpeakers() {
        for (const auto& pair : kBuiltinSpeakers) {
            const std::string utf8_k(pair.jp_utf8);
            const std::string cp932_k = Utf8ToCp932(utf8_k);
            speaker_map.emplace(utf8_k, pair.en_name);
            if (!cp932_k.empty()) {
                speaker_map.emplace(cp932_k, pair.en_name);
            }
        }
    }

    const std::string* InternValue(std::string value) {
        persistent_pool.push_back(std::move(value));
        return &persistent_pool.back();
    }

    void BindKeyVariants(std::string_view raw_key, const std::string* val_ptr) {
        if (raw_key.empty() || !val_ptr) {
            return;
        }
        lookup_map.emplace(std::string(raw_key), val_ptr);
        const std::string norm = NormalizeLookupKey(raw_key);
        if (!norm.empty()) {
            lookup_map.emplace(norm, val_ptr);
        }
    }

    void InsertRow(std::string_view jp_raw, std::string_view en_raw) {
        if (jp_raw.empty() || en_raw.empty()) {
            return;
        }
        const std::string jp_text(jp_raw);
        const std::string en_text(en_raw);

        std::string en_cp932 = IsValidUtf8(en_text)
            ? Utf8ToCp932(en_text)
            : en_text;
        if (en_cp932.empty()) {
            en_cp932 = en_text;
        }

        const std::string* val_ptr = InternValue(std::move(en_cp932));
        ++canonical_entry_count;

        BindKeyVariants(jp_text, val_ptr);
        if (IsValidUtf8(jp_text)) {
            const std::string cp932_key = Utf8ToCp932(jp_text);
            if (!cp932_key.empty()) {
                BindKeyVariants(cp932_key, val_ptr);
                speaker_map[cp932_key] = *val_ptr;
            }
            speaker_map[jp_text] = *val_ptr;
            return;
        }

        const std::string utf8_key = Cp932ToUtf8(jp_text);
        if (!utf8_key.empty()) {
            BindKeyVariants(utf8_key, val_ptr);
        }
    }

    const std::string* FindValuePointer(std::string_view jp_key) const {
        if (jp_key.empty()) {
            return nullptr;
        }
        if (const auto it = lookup_map.find(std::string(jp_key)); it != lookup_map.end()) {
            return it->second;
        }
        const std::string norm = NormalizeLookupKey(jp_key);
        if (!norm.empty()) {
            if (const auto it = lookup_map.find(norm); it != lookup_map.end()) {
                return it->second;
            }
        }
        if (!IsValidUtf8(jp_key)) {
            const std::string as_utf8 = Cp932ToUtf8(jp_key);
            if (!as_utf8.empty()) {
                if (const auto it = lookup_map.find(as_utf8); it != lookup_map.end()) {
                    return it->second;
                }
                const std::string norm_utf8 = NormalizeLookupKey(as_utf8);
                if (!norm_utf8.empty()) {
                    if (const auto it = lookup_map.find(norm_utf8); it != lookup_map.end()) {
                        return it->second;
                    }
                }
            }
            return nullptr;
        }

        const std::string as_cp932 = Utf8ToCp932(jp_key);
        if (!as_cp932.empty()) {
            if (const auto it = lookup_map.find(as_cp932); it != lookup_map.end()) {
                return it->second;
            }
            const std::string norm_cp932 = NormalizeLookupKey(as_cp932);
            if (!norm_cp932.empty()) {
                if (const auto it = lookup_map.find(norm_cp932); it != lookup_map.end()) {
                    return it->second;
                }
            }
        }
        return nullptr;
    }

    std::optional<std::string> FindSpeaker(std::string_view jp_speaker) const {
        if (jp_speaker.empty()) {
            return std::nullopt;
        }
        if (const std::string* ptr = FindValuePointer(jp_speaker)) {
            return *ptr;
        }
        const std::string norm = NormalizeLookupKey(jp_speaker);
        if (const auto it = speaker_map.find(std::string(jp_speaker)); it != speaker_map.end()) {
            return it->second;
        }
        if (!norm.empty()) {
            if (const auto it = speaker_map.find(norm); it != speaker_map.end()) {
                return it->second;
            }
        }
        return std::nullopt;
    }

    std::string TranslateBodyClause(std::string_view raw_body) const {
        if (raw_body.empty()) {
            return {};
        }

        const std::size_t ctrl_len = MeasureLeadingControlPrefix(raw_body);
        const std::string_view leading_ctrl = raw_body.substr(0, ctrl_len);

        const std::string* hit = FindValuePointer(raw_body);
        if (!hit && ctrl_len > 0U) {
            hit = FindValuePointer(raw_body.substr(ctrl_len));
        }
        if (!hit) {
            return std::string(raw_body);
        }

        const std::string& translated = *hit;
        const std::size_t tr_ctrl_len = MeasureLeadingControlPrefix(translated);
        const std::string preserved_prefix =
            (tr_ctrl_len > 0U)
                ? std::string(translated.substr(0, tr_ctrl_len))
                : std::string(leading_ctrl);
        const std::string_view body_to_wrap =
            (tr_ctrl_len > 0U)
                ? std::string_view(translated).substr(tr_ctrl_len)
                : std::string_view(translated);

        const std::string wrapped = WrapProportionalLines(body_to_wrap, max_wrap_columns);
        return preserved_prefix + wrapped;
    }

    std::string TranslateRecord(std::string_view raw_script_str) const {
        if (raw_script_str.empty()) {
            return {};
        }

        if (raw_script_str.front() != kMessageHeaderPrefix) {
            const bool has_trailing_dollar = (raw_script_str.back() == kMessageDelimiterChar);
            const std::string_view body_part = has_trailing_dollar
                ? raw_script_str.substr(0, raw_script_str.size() - 1U)
                : raw_script_str;
            std::string translated_body = TranslateBodyClause(body_part);
            if (has_trailing_dollar) {
                translated_body.push_back(kMessageDelimiterChar);
            }
            return translated_body;
        }

        const std::size_t dollar_idx = raw_script_str.find(kMessageDelimiterChar, 1U);
        if (dollar_idx == std::string_view::npos) {
            return std::string(raw_script_str);
        }

        std::size_t delim_end = dollar_idx + 1U;
        while (delim_end < raw_script_str.size() &&
               (raw_script_str[delim_end] == kMessageDelimiterChar ||
                raw_script_str[delim_end] == kMessageAmpersandChar)) {
            ++delim_end;
        }

        const std::string_view header = raw_script_str.substr(1U, dollar_idx - 1U);
        const std::string_view delimiter =
            raw_script_str.substr(dollar_idx, delim_end - dollar_idx);
        const std::string_view body_with_suffix = raw_script_str.substr(delim_end);

        std::string_view speaker_part = header;
        std::string_view voice_part;
        const bool header_is_utf8 = IsValidUtf8(header);
        const std::size_t bs_idx = FindStandaloneAsciiBackslash(header, header_is_utf8);
        if (bs_idx != std::string_view::npos) {
            speaker_part = header.substr(0, bs_idx);
            voice_part = header.substr(bs_idx);
        }

        std::string translated_speaker(speaker_part);
        if (!speaker_part.empty()) {
            if (const auto spk_hit = FindSpeaker(speaker_part)) {
                translated_speaker = *spk_hit;
            }
        }

        const bool has_trailing_dollar =
            (!body_with_suffix.empty() && body_with_suffix.back() == kMessageDelimiterChar);
        const std::string_view body_raw = has_trailing_dollar
            ? body_with_suffix.substr(0, body_with_suffix.size() - 1U)
            : body_with_suffix;

        const std::string translated_body = TranslateBodyClause(body_raw);

        std::string result;
        result.reserve(1U + translated_speaker.size() + voice_part.size() +
                       delimiter.size() + translated_body.size() + 1U);
        result.push_back(kMessageHeaderPrefix);
        result += translated_speaker;
        result.append(voice_part.data(), voice_part.size());
        result.append(delimiter.data(), delimiter.size());
        result += translated_body;
        if (has_trailing_dollar) {
            result.push_back(kMessageDelimiterChar);
        }
        return result;
    }
};

TranslationCatalog::TranslationCatalog()
    : impl_(std::make_unique<Impl>(kDefaultWrapColumns)) {}

TranslationCatalog::TranslationCatalog(std::size_t max_wrap_columns)
    : impl_(std::make_unique<Impl>(max_wrap_columns)) {}

TranslationCatalog::TranslationCatalog(TranslationCatalog&& other) noexcept = default;
TranslationCatalog& TranslationCatalog::operator=(TranslationCatalog&& other) noexcept = default;
TranslationCatalog::~TranslationCatalog() = default;

std::size_t TranslationCatalog::LoadFromFile(const wchar_t* path) {
    if (!path) {
        return 0;
    }
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
        return 0;
    }
    return LoadFromStream(ifs);
}

std::size_t TranslationCatalog::LoadFromFile(const char* path) {
    if (!path) {
        return 0;
    }
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) {
        return 0;
    }
    return LoadFromStream(ifs);
}

std::size_t TranslationCatalog::LoadFromStream(std::istream& stream) {
    std::string line;
    bool first_line = true;
    const std::size_t before = impl_->canonical_entry_count;

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::string_view sv(line);
        if (first_line) {
            first_line = false;
            if (sv.rfind(kUtf8Bom, 0) == 0) {
                sv.remove_prefix(kUtf8Bom.size());
            }
        }
        if (sv.empty() || sv.front() == '#') {
            continue;
        }
        const std::size_t tab_pos = sv.find('\t');
        if (tab_pos == std::string_view::npos) {
            continue;
        }
        const std::string_view jp_col = sv.substr(0, tab_pos);
        const std::string_view en_col = sv.substr(tab_pos + 1U);
        impl_->InsertRow(jp_col, en_col);
    }

    return impl_->canonical_entry_count - before;
}

std::size_t TranslationCatalog::LoadFromString(std::string_view tsv_content) {
    std::istringstream iss{std::string(tsv_content)};
    return LoadFromStream(iss);
}

std::size_t TranslationCatalog::Size() const noexcept {
    return impl_->canonical_entry_count;
}

std::optional<std::string> TranslationCatalog::LookupTranslation(std::string_view jp_key) const {
    if (const std::string* ptr = impl_->FindValuePointer(jp_key)) {
        return *ptr;
    }
    return std::nullopt;
}

std::optional<std::string> TranslationCatalog::LookupSpeaker(std::string_view jp_speaker) const {
    return impl_->FindSpeaker(jp_speaker);
}

const char* TranslationCatalog::LookupPersistentPointer(std::string_view jp_key) const {
    if (const std::string* ptr = impl_->FindValuePointer(jp_key)) {
        return ptr->c_str();
    }
    return nullptr;
}

std::string TranslationCatalog::TranslateMessageRecord(std::string_view raw_script_str) const {
    return impl_->TranslateRecord(raw_script_str);
}

bool TranslationCatalog::FormatDialogueOrNarration(std::string_view raw_script_str,
                                                   char* out_buf,
                                                   std::size_t out_buf_capacity) const {
    if (!out_buf || out_buf_capacity == 0U) {
        return false;
    }
    const std::string formatted = impl_->TranslateRecord(raw_script_str);
    const std::size_t copy_len = std::min(formatted.size(), out_buf_capacity - 1U);
    std::memcpy(out_buf, formatted.data(), copy_len);
    out_buf[copy_len] = '\0';
    return formatted != raw_script_str;
}

std::string TranslationCatalog::WrapProportionalLines(std::string_view text,
                                                      std::size_t max_columns) {
    if (text.empty() || max_columns == 0U) {
        return std::string(text);
    }

    if (text.find(kMidlinePauseToken) == std::string_view::npos) {
        return WrapBackslashClause(text, max_columns);
    }

    std::string out;
    std::size_t pos = 0;
    bool first = true;
    while (pos <= text.size()) {
        const std::size_t hit = text.find(kMidlinePauseToken, pos);
        const std::string_view clause =
            (hit == std::string_view::npos)
                ? text.substr(pos)
                : text.substr(pos, hit - pos);
        const std::string wrapped_clause = WrapBackslashClause(clause, max_columns);
        if (!first) {
            out.append(kMidlinePauseToken.data(), kMidlinePauseToken.size());
        }
        out += wrapped_clause;
        first = false;
        if (hit == std::string_view::npos) {
            break;
        }
        pos = hit + kMidlinePauseToken.size();
    }
    return out;
}

std::string TranslationCatalog::NormalizeLookupKey(std::string_view raw_key) {
    if (raw_key.empty()) {
        return {};
    }
    const std::size_t ctrl_len = MeasureLeadingControlPrefix(raw_key);
    const std::string_view without_ctrl = raw_key.substr(ctrl_len);
    const std::string without_ruby = StripRubyMarkup(without_ctrl);

    std::string flattened;
    flattened.reserve(without_ruby.size());
    for (std::size_t i = 0; i < without_ruby.size(); ++i) {
        const char c = without_ruby[i];
        if (c == '\r' || c == '\n' || c == '\t') {
            if (!flattened.empty() && flattened.back() != ' ') {
                flattened.push_back(' ');
            }
            continue;
        }
        flattened.push_back(c);
    }
    return TrimLeadingAndTrailingSpaces(flattened);
}

std::string TranslationCatalog::Utf8ToCp932(std::string_view utf8_str) {
    if (utf8_str.empty()) {
        return {};
    }
    const int wide_len = MultiByteToWideChar(
        CP_UTF8, 0, utf8_str.data(), static_cast<int>(utf8_str.size()), nullptr, 0);
    if (wide_len <= 0) {
        return std::string(utf8_str);
    }
    std::wstring wide(static_cast<std::size_t>(wide_len), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, utf8_str.data(), static_cast<int>(utf8_str.size()), wide.data(), wide_len);

    const int cp932_len = WideCharToMultiByte(
        kCodePageShiftJis, 0, wide.data(), wide_len, nullptr, 0, nullptr, nullptr);
    if (cp932_len <= 0) {
        return std::string(utf8_str);
    }
    std::string cp932(static_cast<std::size_t>(cp932_len), '\0');
    WideCharToMultiByte(
        kCodePageShiftJis, 0, wide.data(), wide_len, cp932.data(), cp932_len, nullptr, nullptr);
    return cp932;
}

std::string TranslationCatalog::Cp932ToUtf8(std::string_view cp932_str) {
    if (cp932_str.empty()) {
        return {};
    }
    const int wide_len = MultiByteToWideChar(
        kCodePageShiftJis, 0, cp932_str.data(), static_cast<int>(cp932_str.size()), nullptr, 0);
    if (wide_len <= 0) {
        return std::string(cp932_str);
    }
    std::wstring wide(static_cast<std::size_t>(wide_len), L'\0');
    MultiByteToWideChar(
        kCodePageShiftJis, 0, cp932_str.data(), static_cast<int>(cp932_str.size()), wide.data(), wide_len);

    const int utf8_len = WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), wide_len, nullptr, 0, nullptr, nullptr);
    if (utf8_len <= 0) {
        return std::string(cp932_str);
    }
    std::string utf8(static_cast<std::size_t>(utf8_len), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), wide_len, utf8.data(), utf8_len, nullptr, nullptr);
    return utf8;
}

}  // namespace oretsuba::proxy
