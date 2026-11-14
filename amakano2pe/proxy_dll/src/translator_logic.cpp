// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "translator_logic.h"

#include "ui_text_lookup.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace translator_logic {

namespace {

constexpr std::size_t kControlEscapeMinLen = 2;
constexpr std::size_t kControlThreeCharLen = 3;
constexpr std::size_t kControlFourCharLen = 4;
constexpr std::size_t kWrapReservePadding = 64;

constexpr std::size_t kSubtagOffset1 = 2;
constexpr std::size_t kSubtagOffset2 = 3;

constexpr char kAsciiCaseOffset = 'a' - 'A';
constexpr std::string_view kFullwidthSpaceUtf8 = "\xE3\x80\x80";

constexpr unsigned char kUtf8MaxOneByteLead = 0x7Fu;
constexpr unsigned char kUtf8MinTwoByteLead = 0xC2u;
constexpr unsigned char kUtf8MaxTwoByteLead = 0xDFu;
constexpr unsigned char kUtf8MinThreeByteLead = 0xE0u;
constexpr unsigned char kUtf8MaxThreeByteLead = 0xEFu;
constexpr unsigned char kUtf8MinFourByteLead = 0xF0u;
constexpr unsigned char kUtf8MaxFourByteLead = 0xF4u;

constexpr unsigned char kUtf8ContMask = 0xC0u;
constexpr unsigned char kUtf8ContPrefix = 0x80u;
constexpr unsigned char kUtf8ThreeByteOverlongLead = 0xE0u;
constexpr unsigned char kUtf8MinThreeByteNonOverlongCont = 0xA0u;
constexpr unsigned char kUtf8SurrogateLead = 0xEDu;
constexpr unsigned char kUtf8MaxNonSurrogateCont = 0x9Fu;
constexpr unsigned char kUtf8FourByteOverlongLead = 0xF0u;
constexpr unsigned char kUtf8MinFourByteNonOverlongCont = 0x90u;
constexpr unsigned char kUtf8MaxScalarLead = 0xF4u;
constexpr unsigned char kUtf8MaxScalarCont = 0x8Fu;

constexpr std::size_t kSingleByteLen = 1;
constexpr std::size_t kTwoByteLen = 2;
constexpr std::size_t kThreeByteLen = 3;
constexpr std::size_t kFourByteLen = 4;

constexpr std::size_t kContOffset1 = 1;
constexpr std::size_t kContOffset2 = 2;
constexpr std::size_t kContOffset3 = 3;

constexpr std::size_t kBmpWideUnits = 1;
constexpr std::size_t kSurrogatePairWideUnits = 2;
constexpr std::size_t kNullTerminatorWideUnits = 1;

constexpr std::size_t kUtf8BomLen = 3;
constexpr std::size_t kBomIndex0 = 0;
constexpr std::size_t kBomIndex1 = 1;
constexpr std::size_t kBomIndex2 = 2;
constexpr unsigned char kUtf8Bom0 = 0xEFu;
constexpr unsigned char kUtf8Bom1 = 0xBBu;
constexpr unsigned char kUtf8Bom2 = 0xBFu;

enum class EscapeKind {
    kHardLineBreak,
    kZeroWidthControl,
    kFullwidthSpace,
    kLiteralChar,
};

struct EscapeToken {
    EscapeKind kind = EscapeKind::kLiteralChar;
    std::size_t byte_length = kSingleByteLen;
    char literal_char = '\\';
};

struct Utf8SequenceInfo {
    bool valid = false;
    std::size_t byte_length = kSingleByteLen;
    std::size_t wide_units = kBmpWideUnits;
};

bool IsUtf8ContinuationByte(unsigned char b) noexcept {
    return (b & kUtf8ContMask) == kUtf8ContPrefix;
}

std::size_t ExpectedUtf8SequenceLength(unsigned char lead) noexcept {
    if (lead <= kUtf8MaxOneByteLead) {
        return kSingleByteLen;
    }
    if (lead >= kUtf8MinTwoByteLead && lead <= kUtf8MaxTwoByteLead) {
        return kTwoByteLen;
    }
    if (lead >= kUtf8MinThreeByteLead && lead <= kUtf8MaxThreeByteLead) {
        return kThreeByteLen;
    }
    if (lead >= kUtf8MinFourByteLead && lead <= kUtf8MaxFourByteLead) {
        return kFourByteLen;
    }
    return 0;
}

// Strict RFC 3629 UTF-8 scalar validator at `s[pos]`. Rejects truncated
// sequences, invalid continuation bytes, overlong encodings, UTF-16 surrogates
// (U+D800..U+DFFF), and code points above U+10FFFF. Any ill-formed byte reports
// `byte_length = 1` and `wide_units = 1` (matching U+FFFD under CP_UTF8).
Utf8SequenceInfo InspectUtf8Sequence(std::string_view s,
                                     std::size_t pos) noexcept {
    if (pos >= s.size()) {
        return {false, 0, 0};
    }

    const auto b0 = static_cast<unsigned char>(s[pos]);
    const std::size_t expected_len = ExpectedUtf8SequenceLength(b0);
    if (expected_len == 0 || pos + expected_len > s.size()) {
        return {false, kSingleByteLen, kBmpWideUnits};
    }
    if (expected_len == kSingleByteLen) {
        return {true, kSingleByteLen, kBmpWideUnits};
    }

    const auto b1 = static_cast<unsigned char>(s[pos + kContOffset1]);
    if (!IsUtf8ContinuationByte(b1)) {
        return {false, kSingleByteLen, kBmpWideUnits};
    }
    if (expected_len == kTwoByteLen) {
        return {true, kTwoByteLen, kBmpWideUnits};
    }

    const auto b2 = static_cast<unsigned char>(s[pos + kContOffset2]);
    if (!IsUtf8ContinuationByte(b2)) {
        return {false, kSingleByteLen, kBmpWideUnits};
    }
    if (expected_len == kThreeByteLen) {
        if (b0 == kUtf8ThreeByteOverlongLead &&
            b1 < kUtf8MinThreeByteNonOverlongCont) {
            return {false, kSingleByteLen, kBmpWideUnits};
        }
        if (b0 == kUtf8SurrogateLead && b1 > kUtf8MaxNonSurrogateCont) {
            return {false, kSingleByteLen, kBmpWideUnits};
        }
        return {true, kThreeByteLen, kBmpWideUnits};
    }

    const auto b3 = static_cast<unsigned char>(s[pos + kContOffset3]);
    if (!IsUtf8ContinuationByte(b3)) {
        return {false, kSingleByteLen, kBmpWideUnits};
    }
    if (b0 == kUtf8FourByteOverlongLead &&
        b1 < kUtf8MinFourByteNonOverlongCont) {
        return {false, kSingleByteLen, kBmpWideUnits};
    }
    if (b0 == kUtf8MaxScalarLead && b1 > kUtf8MaxScalarCont) {
        return {false, kSingleByteLen, kBmpWideUnits};
    }
    return {true, kFourByteLen, kSurrogatePairWideUnits};
}

// REQUIRES: s != nullptr && s[pos] != '\0'.
// Inspects at most `expected_len` bytes starting at `s + pos`, stopping
// immediately at the first `'\0'` so it never reads past NUL.
Utf8SequenceInfo InspectCStringUtf8Sequence(const char* s,
                                            std::size_t pos) noexcept {
    const auto b0 = static_cast<unsigned char>(s[pos]);
    const std::size_t expected_len = ExpectedUtf8SequenceLength(b0);
    if (expected_len == 0) {
        return {false, kSingleByteLen, kBmpWideUnits};
    }
    if (expected_len == kSingleByteLen) {
        return {true, kSingleByteLen, kBmpWideUnits};
    }

    std::array<char, kFourByteLen> window{};
    window[0] = s[pos];
    std::size_t window_len = kSingleByteLen;
    while (window_len < expected_len && s[pos + window_len] != '\0') {
        window[window_len] = s[pos + window_len];
        ++window_len;
    }
    if (window_len < expected_len) {
        return {false, kSingleByteLen, kBmpWideUnits};
    }
    return InspectUtf8Sequence(std::string_view(window.data(), window_len), 0);
}

char ToAsciiLower(char c) noexcept {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c + kAsciiCaseOffset);
    }
    return c;
}

bool IsAsciiDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}

bool IsAsciiHexDigit(char c) noexcept {
    if (IsAsciiDigit(c)) {
        return true;
    }
    const char lower = ToAsciiLower(c);
    return lower >= 'a' && lower <= 'f';
}

bool IsAsciiAlphaNum(char c) noexcept {
    if (c >= 'a' && c <= 'z') {
        return true;
    }
    if (c >= 'A' && c <= 'Z') {
        return true;
    }
    return IsAsciiDigit(c);
}

bool IsClauseOrSentencePunctuation(char c) noexcept {
    return c == '.' || c == '!' || c == '?' || c == ',' || c == ';' ||
            c == ':' || c == '~';
}

std::size_t ConsumeOptionalSemicolon(std::string_view s,
                                     std::size_t pos) noexcept {
    if (pos < s.size() && s[pos] == ';') {
        return pos + kSingleByteLen;
    }
    return pos;
}

// Matches CatSystem2 `STRING8::ParseUntilSemicolon` (`0x00627260`), consuming
// characters from `pos` up to and including the first `';'` (or to `s.size()`
// if unterminated).
std::size_t ConsumeUntilAndIncludingSemicolon(std::string_view s,
                                              std::size_t pos) noexcept {
    while (pos < s.size() && s[pos] != ';') {
        ++pos;
    }
    if (pos < s.size() && s[pos] == ';') {
        ++pos;
    }
    return pos;
}

// Matches CatSystem2 `STRING8::ParseUintArg` (`0x00627150`): consumes unsigned
// `0x`/`0X` + `[0-9a-fA-F]*` or `[0-9]+`, followed by an optional `';'`.
// Returns the end index when at least one leading decimal digit (`'0'..'9'`)
// starts at `arg_start`, or 0 without consuming `';'` otherwise.
std::size_t ConsumeUintArg(std::string_view s,
                           std::size_t arg_start) noexcept {
    std::size_t pos = arg_start;
    if (pos + kSingleByteLen < s.size() && s[pos] == '0' &&
        ToAsciiLower(s[pos + kSingleByteLen]) == 'x') {
        pos += kTwoByteLen;
        while (pos < s.size() && IsAsciiHexDigit(s[pos])) {
            ++pos;
        }
        return ConsumeOptionalSemicolon(s, pos);
    }
    const std::size_t digits_start = pos;
    while (pos < s.size() && IsAsciiDigit(s[pos])) {
        ++pos;
    }
    if (pos == digits_start) {
        return 0;
    }
    return ConsumeOptionalSemicolon(s, pos);
}

// Matches CatSystem2's shared `\cr` / `\fr` fallback tail (`0x00626344`) when
// `STRING8::ParseUintArg` returns `<= 0` at `arg_start`: `NextChar()`
// (`0x006272F0`) consumes one character if present, and when that character is
// `';'`, `inc dword ptr [esi + 0x210]` (`0x00626355`) consumes one more
// character if present.
std::size_t ConsumeResetSubtagWithoutDigits(std::string_view s,
                                            std::size_t slash_pos,
                                            std::size_t arg_start) noexcept {
    if (arg_start >= s.size()) {
        return arg_start - slash_pos;
    }
    const bool has_semicolon = (s[arg_start] == ';');
    std::size_t pos = arg_start + InspectUtf8Sequence(s, arg_start).byte_length;
    if (has_semicolon && pos < s.size()) {
        pos += InspectUtf8Sequence(s, pos).byte_length;
    }
    return pos - slash_pos;
}

// Matches CatSystem2 `\id` (`0x006268D0`..`0x0062691D`): parses the first
// unsigned argument (including any optional `';'`), then inspects the next
// character via `NextChar()` (`0x006268F5`). At exact EOF (`first_end ==
// s.size()`), `NextChar()` returns 0 without advancing and `dec [esi + 0x210]`
// (`0x00626917`) steps back 1 byte; when the next character is `':'`, consumes
// `':'` plus an optional second unsigned argument without stepping back.
std::size_t ConsumeIdEscapeLength(std::string_view s,
                                  std::size_t slash_pos) noexcept {
    const std::size_t first_end =
        ConsumeUintArg(s, slash_pos + kControlThreeCharLen);
    if (first_end == 0) {
        return kControlThreeCharLen;
    }
    if (first_end == s.size()) {
        return (first_end - kSingleByteLen) - slash_pos;
    }
    if (s[first_end] != ':') {
        return first_end - slash_pos;
    }
    const std::size_t after_colon = first_end + kSingleByteLen;
    const std::size_t second_end = ConsumeUintArg(s, after_colon);
    const std::size_t end = (second_end > 0) ? second_end : after_colon;
    return end - slash_pos;
}

// Parses a CatSystem2 STRING8/STRING16 backslash sequence starting at
// `slash_pos` (precondition: `s[slash_pos] == '\\'`). Matches
// `STRING8::ParseTokens` (`0x0062629E`), `STRING16::ParseOneEscape`
// (`0x0062C260`), and `ParseUintArg` (`0x00627150`) case-insensitively across
// all control tags.
EscapeToken ConsumeCatSystem2Escape(std::string_view s,
                                    std::size_t slash_pos) noexcept {
    if (slash_pos + kSingleByteLen >= s.size()) {
        return {EscapeKind::kLiteralChar, kSingleByteLen, '\\'};
    }

    const char tag = s[slash_pos + kSingleByteLen];
    const char lower_tag = ToAsciiLower(tag);

    if (lower_tag == 'n') {
        return {EscapeKind::kHardLineBreak, kControlEscapeMinLen, '\0'};
    }
    if (tag == '@' || lower_tag == 'r') {
        return {EscapeKind::kZeroWidthControl, kControlEscapeMinLen, '\0'};
    }
    if (lower_tag == 's') {
        return {EscapeKind::kFullwidthSpace, kControlEscapeMinLen, '\0'};
    }
    if (lower_tag == 'p') {
        if (slash_pos + kSubtagOffset1 == s.size()) {
            return {EscapeKind::kZeroWidthControl, kSingleByteLen, '\0'};
        }
        const char align = ToAsciiLower(s[slash_pos + kSubtagOffset1]);
        if (align == 'l' || align == 'c' || align == 'r') {
            return {EscapeKind::kZeroWidthControl, kControlThreeCharLen, '\0'};
        }
        return {EscapeKind::kZeroWidthControl, kControlEscapeMinLen, '\0'};
    }
    if (lower_tag == 'f') {
        if (slash_pos + kSubtagOffset1 == s.size()) {
            return {EscapeKind::kZeroWidthControl, kSingleByteLen, '\0'};
        }
        if (slash_pos + kSubtagOffset2 < s.size()) {
            const char p1 = ToAsciiLower(s[slash_pos + kSubtagOffset1]);
            const char p2 = ToAsciiLower(s[slash_pos + kSubtagOffset2]);
            if ((p1 == 's' && p2 == 's') || (p1 == 'l' && p2 == 'l')) {
                const std::size_t end = ConsumeOptionalSemicolon(
                    s, slash_pos + kControlFourCharLen);
                return {EscapeKind::kZeroWidthControl, end - slash_pos, '\0'};
            }
        }
        const char sub = ToAsciiLower(s[slash_pos + kSubtagOffset1]);
        if (sub == 's' || sub == 'l') {
            const std::size_t end = ConsumeOptionalSemicolon(
                s, slash_pos + kControlThreeCharLen);
            return {EscapeKind::kZeroWidthControl, end - slash_pos, '\0'};
        }
        if (sub == 'n') {
            return {EscapeKind::kZeroWidthControl, kControlThreeCharLen, '\0'};
        }
        if (sub == 'r') {
            const std::size_t arg_start = slash_pos + kControlThreeCharLen;
            const std::size_t arg_end = ConsumeUintArg(s, arg_start);
            if (arg_end > 0) {
                return {EscapeKind::kZeroWidthControl,
                        arg_end - slash_pos, '\0'};
            }
            return {EscapeKind::kZeroWidthControl,
                    ConsumeResetSubtagWithoutDigits(s, slash_pos, arg_start),
                    '\0'};
        }
        if (sub == '+' || sub == '-') {
            const std::size_t arg_end =
                ConsumeUintArg(s, slash_pos + kControlThreeCharLen);
            if (arg_end > 0) {
                return {EscapeKind::kZeroWidthControl,
                        arg_end - slash_pos, '\0'};
            }
            return {EscapeKind::kZeroWidthControl, kControlThreeCharLen, '\0'};
        }
        const std::size_t arg_end =
            ConsumeUintArg(s, slash_pos + kSubtagOffset1);
        if (arg_end > 0) {
            return {EscapeKind::kZeroWidthControl, arg_end - slash_pos, '\0'};
        }
        const std::size_t end =
            ConsumeOptionalSemicolon(s, slash_pos + kSubtagOffset1);
        return {EscapeKind::kZeroWidthControl, end - slash_pos, '\0'};
    }
    if (lower_tag == 'w') {
        if (slash_pos + kSubtagOffset1 == s.size()) {
            return {EscapeKind::kZeroWidthControl, kSingleByteLen, '\0'};
        }
        const char sub = ToAsciiLower(s[slash_pos + kSubtagOffset1]);
        if (sub == 'f') {
            const std::size_t arg_end =
                ConsumeUintArg(s, slash_pos + kControlThreeCharLen);
            if (arg_end > 0) {
                return {EscapeKind::kZeroWidthControl,
                        arg_end - slash_pos, '\0'};
            }
            return {EscapeKind::kZeroWidthControl, kControlThreeCharLen, '\0'};
        }
        if (sub == 'r') {
            return {EscapeKind::kZeroWidthControl, kControlThreeCharLen, '\0'};
        }
        const std::size_t arg_end =
            ConsumeUintArg(s, slash_pos + kSubtagOffset1);
        if (arg_end > 0) {
            return {EscapeKind::kZeroWidthControl, arg_end - slash_pos, '\0'};
        }
        return {EscapeKind::kZeroWidthControl, kControlEscapeMinLen, '\0'};
    }
    if (lower_tag == 'c') {
        if (slash_pos + kSubtagOffset1 < s.size()) {
            const char sub = ToAsciiLower(s[slash_pos + kSubtagOffset1]);
            if (sub == 'r') {
                const std::size_t arg_start = slash_pos + kControlThreeCharLen;
                const std::size_t arg_end = ConsumeUintArg(s, arg_start);
                if (arg_end > 0) {
                    return {EscapeKind::kZeroWidthControl,
                            arg_end - slash_pos, '\0'};
                }
                return {
                    EscapeKind::kZeroWidthControl,
                    ConsumeResetSubtagWithoutDigits(s, slash_pos, arg_start),
                    '\0'};
            }
            if (sub == 's' || sub == 'd') {
                const std::size_t arg_end =
                    ConsumeUintArg(s, slash_pos + kControlThreeCharLen);
                if (arg_end > 0) {
                    return {EscapeKind::kZeroWidthControl,
                            arg_end - slash_pos, '\0'};
                }
                return {EscapeKind::kZeroWidthControl, kControlThreeCharLen, '\0'};
            }
        }
        const std::size_t arg_end =
            ConsumeUintArg(s, slash_pos + kSubtagOffset1);
        if (arg_end > 0) {
            return {EscapeKind::kZeroWidthControl, arg_end - slash_pos, '\0'};
        }
        const std::size_t end =
            ConsumeOptionalSemicolon(s, slash_pos + kSubtagOffset1);
        return {EscapeKind::kZeroWidthControl, end - slash_pos, '\0'};
    }
    if (lower_tag == 'x' || lower_tag == 'y') {
        const std::size_t end =
            ConsumeUintArg(s, slash_pos + kSubtagOffset1);
        if (end > 0) {
            return {EscapeKind::kZeroWidthControl, end - slash_pos, '\0'};
        }
        return {EscapeKind::kZeroWidthControl, kControlEscapeMinLen, '\0'};
    }
    if (lower_tag == 'i') {
        if (slash_pos + kSubtagOffset1 < s.size() &&
            ToAsciiLower(s[slash_pos + kSubtagOffset1]) == 'd') {
            return {EscapeKind::kZeroWidthControl,
                    ConsumeIdEscapeLength(s, slash_pos), '\0'};
        }
        return {EscapeKind::kZeroWidthControl, kControlEscapeMinLen, '\0'};
    }
    if (lower_tag == 't') {
        std::size_t pos = slash_pos + kSubtagOffset1;
        while (pos < s.size()) {
            const char flag = ToAsciiLower(s[pos]);
            if (flag != 'b' && flag != 'i') {
                break;
            }
            ++pos;
        }
        if (pos == s.size()) {
            return {EscapeKind::kZeroWidthControl,
                    (pos - kSingleByteLen) - slash_pos, '\0'};
        }
        pos = ConsumeOptionalSemicolon(s, pos);
        return {EscapeKind::kZeroWidthControl, pos - slash_pos, '\0'};
    }
    if (lower_tag == 'u') {
        if (slash_pos + kSubtagOffset1 < s.size() &&
            ToAsciiLower(s[slash_pos + kSubtagOffset1]) == 'i') {
            const std::size_t arg_end =
                ConsumeUintArg(s, slash_pos + kControlThreeCharLen);
            if (arg_end > 0) {
                return {EscapeKind::kZeroWidthControl, arg_end - slash_pos,
                        '\0'};
            }
            return {EscapeKind::kZeroWidthControl, kControlThreeCharLen, '\0'};
        }
        return {EscapeKind::kZeroWidthControl, kControlEscapeMinLen, '\0'};
    }
    if (lower_tag == 'z' || tag == '$') {
        const std::size_t end =
            ConsumeUntilAndIncludingSemicolon(s, slash_pos + kSubtagOffset1);
        return {EscapeKind::kZeroWidthControl, end - slash_pos, '\0'};
    }

    return {EscapeKind::kLiteralChar, kControlEscapeMinLen, tag};
}

bool IsHardBreakAt(std::string_view s, std::size_t pos) noexcept {
    if (pos >= s.size()) {
        return false;
    }
    if (s[pos] == '\n') {
        return true;
    }
    return s[pos] == '\\' && pos + kSingleByteLen < s.size() &&
           ToAsciiLower(s[pos + kSingleByteLen]) == 'n';
}

// Advances from `start` to the end of the non-space, non-linebreak token,
// keeping CatSystem2 backslash escapes intact so a control sequence or
// escaped character is never split across tokens.
std::size_t ScanWordTokenEnd(std::string_view s, std::size_t start) noexcept {
    std::size_t i = start;
    while (i < s.size() && s[i] != ' ' && !IsHardBreakAt(s, i)) {
        if (s[i] == '\\') {
            const EscapeToken esc = ConsumeCatSystem2Escape(s, i);
            i += esc.byte_length;
            continue;
        }
        ++i;
    }
    return i;
}

// Produces the visible characters of `text[token_start..token_end)` by
// stripping zero-width CatSystem2 control tags (`\@`, `\pc`, `\f+20;`, `\w0;`,
// etc.), expanding `\s`/`\S` to fullwidth space U+3000, and resolving literal
// escapes (`\"` -> `"`). Evaluates escapes against the full `text` buffer so
// exact-EOF cursor backsteps apply only at `text.size()`, never at an internal
// space or line-break token boundary.
std::string ExtractVisibleText(std::string_view text,
                               std::size_t token_start,
                               std::size_t token_end) {
    const std::string_view raw_token =
        text.substr(token_start, token_end - token_start);
    if (raw_token.find('\\') == std::string_view::npos) {
        return std::string(raw_token);
    }

    std::string visible;
    visible.reserve(raw_token.size());
    std::size_t i = token_start;
    while (i < token_end) {
        if (text[i] != '\\') {
            visible.push_back(text[i]);
            ++i;
            continue;
        }
        const EscapeToken esc = ConsumeCatSystem2Escape(text, i);
        if (esc.kind == EscapeKind::kFullwidthSpace) {
            visible.append(kFullwidthSpaceUtf8.data(),
                           kFullwidthSpaceUtf8.size());
        }
        if (esc.kind == EscapeKind::kLiteralChar) {
            visible.push_back(esc.literal_char);
        }
        i += esc.byte_length;
    }
    return visible;
}

std::size_t MeasureTokenVisibleWidth(std::string_view text,
                                     std::size_t token_start,
                                     std::size_t token_end,
                                     const TextWidthMeasurer& measurer) {
    if (token_start >= token_end) {
        return 0;
    }
    const std::string_view raw_token =
        text.substr(token_start, token_end - token_start);
    if (raw_token.find('\\') == std::string_view::npos) {
        return measurer.MeasureSpanWidth(raw_token);
    }
    const std::string visible =
        ExtractVisibleText(text, token_start, token_end);
    if (visible.empty()) {
        return 0;
    }
    return measurer.MeasureSpanWidth(visible);
}

char FirstVisibleChar(std::string_view s) noexcept {
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '\n') {
            return '\0';
        }
        if (s[i] != '\\') {
            return s[i];
        }
        const EscapeToken esc = ConsumeCatSystem2Escape(s, i);
        if (esc.kind == EscapeKind::kHardLineBreak) {
            return '\0';
        }
        if (esc.kind == EscapeKind::kFullwidthSpace) {
            return kFullwidthSpaceUtf8[0];
        }
        if (esc.kind == EscapeKind::kLiteralChar) {
            return esc.literal_char;
        }
        i += esc.byte_length;
    }
    return '\0';
}

char LastVisibleCharOnFinalLine(std::string_view s) noexcept {
    char last = '\0';
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '\n') {
            last = '\0';
            ++i;
            continue;
        }
        if (s[i] != '\\') {
            last = s[i];
            ++i;
            continue;
        }
        const EscapeToken esc = ConsumeCatSystem2Escape(s, i);
        switch (esc.kind) {
        case EscapeKind::kHardLineBreak:
            last = '\0';
            break;
        case EscapeKind::kFullwidthSpace:
            last = ' ';
            break;
        case EscapeKind::kLiteralChar:
            last = esc.literal_char;
            break;
        case EscapeKind::kZeroWidthControl:
            break;
        }
        i += esc.byte_length;
    }
    return last;
}

bool ContainsCatSystem2LineBreak(std::string_view s) noexcept {
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '\n') {
            return true;
        }
        if (s[i] != '\\') {
            ++i;
            continue;
        }
        const EscapeToken esc = ConsumeCatSystem2Escape(s, i);
        if (esc.kind == EscapeKind::kHardLineBreak) {
            return true;
        }
        i += esc.byte_length;
    }
    return false;
}

char NextPrevLastVisibleChar(std::size_t current_width,
                             char prev_last_visible_char,
                             char segment_first_visible_char,
                             std::string_view wrapped) noexcept {
    if (current_width == 0) {
        return '\0';
    }
    if (segment_first_visible_char == '\0' &&
        !ContainsCatSystem2LineBreak(wrapped)) {
        return prev_last_visible_char;
    }
    return LastVisibleCharOnFinalLine(wrapped);
}

// Returns true iff `s[pos..)` reaches an explicit line break (`\n`, `\N`, or
// literal '\n') before encountering any token with non-zero visible width.
bool LeadsToExplicitHardBreakWithZeroWidth(
    std::string_view s,
    std::size_t pos,
    const TextWidthMeasurer& measurer) {
    while (pos < s.size()) {
        if (IsHardBreakAt(s, pos)) {
            return true;
        }
        if (s[pos] == ' ') {
            ++pos;
            continue;
        }
        const std::size_t end = ScanWordTokenEnd(s, pos);
        if (MeasureTokenVisibleWidth(s, pos, end, measurer) > 0) {
            return false;
        }
        pos = end;
    }
    return false;
}

bool ShouldWrapBeforeToken(std::size_t width,
                           std::size_t start_width,
                           std::size_t max_width,
                           std::size_t token_start,
                           std::size_t word_width) noexcept {
    if (word_width == 0) {
        return false;
    }
    if (width > 0 && width + word_width > max_width) {
        return true;
    }
    return width > max_width && width == start_width && token_start == 0;
}

}  // namespace

TextWidthMeasurer::~TextWidthMeasurer() = default;

MonospaceByteWidthMeasurer::~MonospaceByteWidthMeasurer() = default;

std::size_t MonospaceByteWidthMeasurer::MeasureSpanWidth(
    std::string_view utf8_span) const {
    return utf8_span.size();
}

CodePointWidthMeasurer::CodePointWidthMeasurer(
    std::size_t width_per_codepoint) noexcept
    : width_per_codepoint_(width_per_codepoint) {}

CodePointWidthMeasurer::~CodePointWidthMeasurer() = default;

std::size_t CodePointWidthMeasurer::MeasureSpanWidth(
    std::string_view utf8_span) const {
    std::size_t count = 0;
    std::size_t i = 0;
    while (i < utf8_span.size()) {
        const Utf8SequenceInfo seq = InspectUtf8Sequence(utf8_span, i);
        i += seq.byte_length;
        ++count;
    }
    return count * width_per_codepoint_;
}

void UnescapeInPlace(std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + kSingleByteLen < s.size()) {
            const char c = s[i + kSingleByteLen];
            if (c == 'r') {
                out += '\r';
                ++i;
                continue;
            }
            if (c == 'n') {
                out += '\n';
                ++i;
                continue;
            }
            if (c == 't') {
                out += '\t';
                ++i;
                continue;
            }
            if (c == '\\') {
                out += '\\';
                ++i;
                continue;
            }
        }
        out += s[i];
    }
    s = std::move(out);
}

std::size_t Utf8SafeTruncate(const char* s, std::size_t max_bytes) {
    if (s == nullptr || max_bytes == 0) {
        return 0;
    }
    std::size_t i = 0;
    while (i < max_bytes && s[i] != '\0') {
        const Utf8SequenceInfo seq = InspectCStringUtf8Sequence(s, i);
        if (!seq.valid || i + seq.byte_length > max_bytes) {
            break;
        }
        i += seq.byte_length;
    }
    return i;
}

std::size_t Utf8SafeTruncateForWideLimit(std::string_view utf8,
                                         std::size_t max_bytes,
                                         std::size_t max_wchars) {
    if (utf8.empty() || max_bytes == 0) {
        return 0;
    }
    if (max_wchars == kNullTerminatorWideUnits) {
        return 0;
    }

    const std::size_t byte_limit = std::min(utf8.size(), max_bytes);
    const bool enforce_wide_limit = max_wchars > kNullTerminatorWideUnits;
    const std::size_t wide_limit =
        enforce_wide_limit ? (max_wchars - kNullTerminatorWideUnits) : 0;

    std::size_t byte_pos = 0;
    std::size_t wide_used = 0;
    while (byte_pos < byte_limit) {
        const Utf8SequenceInfo seq = InspectUtf8Sequence(utf8, byte_pos);
        if (byte_pos + seq.byte_length > byte_limit) {
            break;
        }
        if (enforce_wide_limit && wide_used + seq.wide_units > wide_limit) {
            break;
        }
        byte_pos += seq.byte_length;
        wide_used += seq.wide_units;
    }
    return byte_pos;
}

bool StripLeadingControlNewline(std::string& s) {
    if (s.size() >= kControlEscapeMinLen && s[0] == '\\' &&
        ToAsciiLower(s[kSingleByteLen]) == 'n') {
        s.replace(0, kControlEscapeMinLen, " ");
        return true;
    }
    return false;
}

bool EndsWithInlineWaitTag(std::string_view s) noexcept {
    if (s.size() < kControlEscapeMinLen || s.back() != '@') {
        return false;
    }
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '\\') {
            i += InspectUtf8Sequence(s, i).byte_length;
            continue;
        }
        if (i + kControlEscapeMinLen == s.size() &&
            s[i + kSingleByteLen] == '@') {
            return true;
        }
        i += ConsumeCatSystem2Escape(s, i).byte_length;
    }
    return false;
}

bool HasDanglingTrailingBackslash(std::string_view s) noexcept {
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '\\') {
            i += InspectUtf8Sequence(s, i).byte_length;
            continue;
        }
        if (i + kSingleByteLen == s.size()) {
            return true;
        }
        i += ConsumeCatSystem2Escape(s, i).byte_length;
    }
    return false;
}

void StripDanglingTrailingBackslash(std::string& s) noexcept {
    if (HasDanglingTrailingBackslash(s)) {
        s.pop_back();
    }
}

std::size_t WordWrapByWidth(std::string& s, std::size_t max_width,
                            std::size_t start_width,
                            const TextWidthMeasurer& measurer) {
    StripDanglingTrailingBackslash(s);
    std::string out;
    out.reserve(s.size() + kWrapReservePadding);
    const std::string_view view(s);
    std::size_t width = start_width;
    bool auto_wrapped_line = false;
    std::size_t i = 0;

    while (i < view.size()) {
        if (view[i] == '\n') {
            out += '\n';
            ++i;
            width = 0;
            auto_wrapped_line = false;
            continue;
        }

        if (view[i] == '\\' && i + kSingleByteLen < view.size() &&
            ToAsciiLower(view[i + kSingleByteLen]) == 'n') {
            out += '\\';
            out += view[i + kSingleByteLen];
            i += kControlEscapeMinLen;
            width = 0;
            auto_wrapped_line = false;
            continue;
        }

        if (view[i] == ' ') {
            std::size_t sp = 0;
            while (i + sp < view.size() && view[i + sp] == ' ') {
                ++sp;
            }
            if (width == 0 && auto_wrapped_line) {
                i += sp;
                continue;
            }
            const std::string_view space_span = view.substr(i, sp);
            const std::size_t space_width = measurer.MeasureSpanWidth(space_span);

            const std::size_t wstart = i + sp;
            if (width > 0 && width + space_width > max_width &&
                LeadsToExplicitHardBreakWithZeroWidth(view, wstart, measurer)) {
                i += sp;
                continue;
            }

            const std::size_t wend = ScanWordTokenEnd(view, wstart);
            const std::size_t word_width =
                MeasureTokenVisibleWidth(view, wstart, wend, measurer);

            if (width > 0 && width + space_width + word_width > max_width) {
                out += '\\';
                out += 'n';
                width = 0;
                auto_wrapped_line = true;
                i += sp;
                continue;
            }

            out.append(space_span.data(), space_span.size());
            width += space_width;
            if (space_width > 0) {
                auto_wrapped_line = false;
            }
            i += sp;
            continue;
        }

        const std::size_t wstart = i;
        const std::size_t wend = ScanWordTokenEnd(view, wstart);
        const std::string_view token = view.substr(wstart, wend - wstart);
        const std::size_t word_width =
            MeasureTokenVisibleWidth(view, wstart, wend, measurer);

        if (ShouldWrapBeforeToken(width, start_width, max_width, wstart,
                                  word_width)) {
            out += '\\';
            out += 'n';
            width = 0;
            auto_wrapped_line = true;
        }

        out.append(token.data(), token.size());
        width += word_width;
        if (word_width > 0) {
            auto_wrapped_line = false;
        }
        i = wend;
    }

    StripDanglingTrailingBackslash(out);
    s = std::move(out);
    return width;
}

std::size_t WordWrapForEngine(std::string& s, std::size_t line_chars,
                              std::size_t start_col) {
    const MonospaceByteWidthMeasurer measurer;
    return WordWrapByWidth(s, line_chars, start_col, measurer);
}

// Complete set of UTF-8 speaker-name (`NAME`) keys present in
// `translation_table.tsv`: all 102 translated Japanese `NAME` strings from
// `scene.int` plus the 2 `$str20 -> 賢一` runtime-resolved protagonist keys
// (`賢一` and `ちとせ＆賢一`) emitted by `02_build_tsv`. Untranslated 4-letter
// ASCII voice-folder IDs (`Chit`..`Yuzu`) and raw `$str20` placeholders with
// `jp == en` are omitted from `translation_table.tsv`. Zero keys overlap with
// `MESSAGE` strings.
const SpeakerKeySet& DefaultSpeakerNameKeys() {
    static const SpeakerKeySet* const kKeys = new SpeakerKeySet{
        "おじさん",  // Uncle
        "おばあさん",  // Grandmother
        "おばさん",  // Aunt
        "お客さん",  // Customer
        "こはる＠店員さん",  // Koharu (Clerk)
        "こはる＠観光客さんＢ",  // Koharu (Tourist B)
        "ちとせ",  // Chitose
        "ちとせ＆$str20",  // Chitose & $str20
        "ちとせ＆賢一",  // Chitose & Kenichi
        "ちとせ＠二人",  // Chitose (Both)
        "ちとせ＠？？？",  // Chitose (???)
        "ゆうひ",  // Yuuhi
        "ジャック",  // Jack
        "ゾンビ",  // Zombie
        "プリント機",  // Printer
        "マスター",  // Master
        "三人",  // Three of Them
        "副部長",  // Vice President
        "副部長＠茉莉",  // Vice President (Mari)
        "吸血鬼",  // Vampire
        "国語教師",  // Japanese Teacher
        "奏＠観光客さんＡ",  // Kanade (Tourist A)
        "奏＠観光客さんＢ",  // Kanade (Tourist B)
        "女子下級生Ａ",  // Female Underclassman A
        "女子下級生Ｂ",  // Female Underclassman B
        "女子下級生Ｃ",  // Female Underclassman C
        "女子下級生Ｄ",  // Female Underclassman D
        "女子先輩Ａ",  // Female Senior A
        "女子先輩Ｂ",  // Female Senior B
        "女子学生",  // Female Student
        "女子学生たち",  // Female Students
        "女子学生Ａ",  // Female Student A
        "女子学生Ｂ",  // Female Student B
        "女子学生Ｃ",  // Female Student C
        "女子学生Ｄ",  // Female Student D
        "女子部員たち",  // Female Club Members
        "女子部員Ａ",  // Female Club Member A
        "女子部員Ｂ",  // Female Club Member B
        "女子部員Ｃ",  // Female Club Member C
        "女将さん",  // Hostess
        "女将さん＠おばあさん",  // Hostess (Grandmother)
        "女性俳優",  // Female Actress
        "女性客",  // Female Customer
        "学生たち",  // Students
        "寶泉路",  // Housenji
        "寶泉路＠？？？",  // Housenji (???)
        "少女",  // Girl
        "少年",  // Boy
        "店員さん",  // Clerk
        "担任",  // Homeroom Teacher
        "放送",  // Broadcast
        "教員",  // Teacher
        "数学教師",  // Math Teacher
        "旦那さん",  // Husband
        "歴史教師",  // History Teacher
        "母さん",  // Mom
        "涙香＠観光客さんＡ",  // Ruika (Tourist A)
        "淵上",  // Fuchinoe
        "淵上＠？？？",  // Fuchinoe (???)
        "猫",  // Cat
        "玲",  // Rei
        "玲の父",  // Rei's Father
        "玲＠おばけ",  // Rei (Ghost)
        "玲＠玲の声",  // Rei (Rei's Voice)
        "玲＠静かな声",  // Rei (Quiet Voice)
        "玲＠？？？",  // Rei (???)
        "男の子",  // Boy
        "男の子の母",  // Boy's Mother
        "男の子の母親",  // Boy's Mother
        "男の子の父親",  // Boy's Father
        "男子下級生Ａ",  // Male Underclassman A
        "男子学生",  // Male Student
        "男子学生Ａ",  // Male Student A
        "男子学生Ｂ",  // Male Student B
        "男子学生Ｃ",  // Male Student C
        "男性",  // Man
        "男性俳優",  // Male Actor
        "男性客",  // Male Customer
        "社会教師",  // Social Studies Teacher
        "社会科教師",  // Social Studies Teacher
        "穂波＠お客さんＢ",  // Honami (Customer B)
        "穂波＠観光客さんＡ",  // Honami (Tourist A)
        "紗雪＠巫女さんＡ",  // Sayuki (Shrine Maiden A)
        "紗雪＠観光客さんＢ",  // Sayuki (Tourist B)
        "結灯",  // Yuuhi
        "結灯の父",  // Yuuhi's Father
        "結灯の父＠？？？",  // Yuuhi's Father (???)
        "結灯＠少女",  // Yuuhi (Girl)
        "結灯＠店員さん",  // Yuuhi (Clerk)
        "結灯＠店員さんの声",  // Yuuhi (Clerk's Voice)
        "結灯＠？？？",  // Yuuhi (???)
        "聖＠お客さんＡ",  // Mizuki (Customer A)
        "聖＠観光客さんＢ",  // Mizuki (Tourist B)
        "英語教師",  // English Teacher
        "誰かの声",  // Someone's Voice
        "賢一",  // Kenichi
        "迷子の女の子",  // Lost Girl
        "通行人Ａ",  // Passerby A
        "通行人Ｂ",  // Passerby B
        "通行人Ｃ",  // Passerby C
        "通行人Ｄ",  // Passerby D
        "部員たち",  // Club Members
        "雪静＠巫女さんＢ",  // Yuzuka (Shrine Maiden B)
        "？？？",  // ???
    };
    return *kKeys;
}

struct DialogueTranslationSession::Impl {
    const TranslationMap* translations = nullptr;
    const TextWidthMeasurer* measurer = nullptr;
    DialogueSessionConfig config{};
    SpeakerKeySet speaker_name_keys = DefaultSpeakerNameKeys();

    std::size_t current_width = 0;
    bool prev_ended_with_inline_wait = false;
    char prev_last_visible_char = '\0';

    std::string last_dialogue_src_key;
    std::string last_dialogue_wrapped;
    std::size_t last_start_width = 0;
    bool last_is_continuation = false;
};

DialogueTranslationSession::DialogueTranslationSession(
    const TranslationMap* translations,
    const TextWidthMeasurer* measurer,
    DialogueSessionConfig config)
    : impl_(std::make_unique<Impl>()) {
    impl_->translations = translations;
    impl_->measurer = measurer;
    impl_->config = config;
}

DialogueTranslationSession::~DialogueTranslationSession() = default;

DialogueTranslationSession::DialogueTranslationSession(
    DialogueTranslationSession&&) noexcept = default;

DialogueTranslationSession& DialogueTranslationSession::operator=(
    DialogueTranslationSession&&) noexcept = default;

void DialogueTranslationSession::SetMaxLineWidth(
    std::size_t max_line_width) noexcept {
    impl_->config.max_line_width = max_line_width;
}

void DialogueTranslationSession::SetSpeakerNameKeys(
    SpeakerKeySet speaker_name_keys) {
    impl_->speaker_name_keys = std::move(speaker_name_keys);
}

std::size_t DialogueTranslationSession::MaxLineWidth() const noexcept {
    return impl_->config.max_line_width;
}

std::size_t DialogueTranslationSession::CurrentLineWidth() const noexcept {
    return impl_->current_width;
}

bool DialogueTranslationSession::PrevEndedWithInlineWait() const noexcept {
    return impl_->prev_ended_with_inline_wait;
}

void DialogueTranslationSession::ResetDialogueState() noexcept {
    impl_->current_width = 0;
    impl_->prev_ended_with_inline_wait = false;
    impl_->prev_last_visible_char = '\0';
    impl_->last_dialogue_src_key.clear();
    impl_->last_dialogue_wrapped.clear();
    impl_->last_start_width = 0;
    impl_->last_is_continuation = false;
}

TranslationResult DialogueTranslationSession::Translate(
    std::string_view src, std::size_t max_wchars) {
    TranslationResult result;
    if (impl_->translations == nullptr || impl_->measurer == nullptr ||
        src.empty()) {
        return result;
    }

    // CatSystem2 STRING8::ParseTokens invokes FUN_0063a9b0 with max_wchars == 16
    // to decode individual glyphs one character at a time. Never substitute
    // dictionary entries inside single-character glyph tokenization.
    if (max_wchars > 0 &&
        max_wchars <= impl_->config.single_char_passthru_wchars) {
        return result;
    }

    const std::string key(src);
    if (impl_->speaker_name_keys.count(key) > 0 &&
        max_wchars <= impl_->config.max_speaker_wchars) {
        const std::string* en = FindTranslation(*impl_->translations, key);
        if (en != nullptr) {
            const std::size_t keep = Utf8SafeTruncateForWideLimit(
                *en, impl_->config.max_output_bytes, max_wchars);
            result.translated = true;
            result.is_speaker_name = true;
            result.truncated = keep < en->size();
            result.text.assign(en->data(), keep);
            return result;
        }
    }

    if (const auto ui_en = LookupStatelessUiText(src); ui_en.has_value()) {
        const std::size_t keep = Utf8SafeTruncateForWideLimit(
            *ui_en, impl_->config.max_output_bytes, max_wchars);
        result.translated = true;
        result.is_ui_text = true;
        result.truncated = keep < ui_en->size();
        result.text.assign(ui_en->data(), keep);
        StripDanglingTrailingBackslash(result.text);
        return result;
    }

    const std::string* en = FindTranslation(*impl_->translations, key);
    if (en == nullptr) {
        // Standalone `\n` MESSAGE (e.g. trailing newline after `\@` before INPUT)
        // resets the running line width and inline-wait carry.
        if (src == "\\n" || src == "\\N" || src == "\n") {
            impl_->current_width = 0;
            impl_->prev_ended_with_inline_wait = false;
            impl_->prev_last_visible_char = '\0';
            impl_->last_dialogue_src_key.clear();
        }
        return result;
    }

    // Layout pass + draw pass deduplication: consecutive calls for the same
    // Japanese MESSAGE reuse the wrapped English text without advancing
    // current_width a second time.
    if (!impl_->last_dialogue_src_key.empty() &&
        src == impl_->last_dialogue_src_key) {
        const std::size_t keep = Utf8SafeTruncateForWideLimit(
            impl_->last_dialogue_wrapped, impl_->config.max_output_bytes,
            max_wchars);
        result.translated = true;
        result.deduplicated = true;
        result.is_continuation = impl_->last_is_continuation;
        result.start_width = impl_->last_start_width;
        result.end_width = impl_->current_width;
        result.truncated = keep < impl_->last_dialogue_wrapped.size();
        result.text.assign(impl_->last_dialogue_wrapped.data(), keep);
        StripDanglingTrailingBackslash(result.text);
        return result;
    }

    std::string wrapped = *en;
    StripDanglingTrailingBackslash(wrapped);
    bool stripped_leading_nl = false;
    if (wrapped.size() >= kControlEscapeMinLen && wrapped[0] == '\\' &&
        ToAsciiLower(wrapped[kSingleByteLen]) == 'n') {
        wrapped.erase(0, kControlEscapeMinLen);
        stripped_leading_nl = true;
    }

    const bool is_continuation =
        stripped_leading_nl || impl_->prev_ended_with_inline_wait;
    if (!is_continuation) {
        impl_->current_width = 0;
        impl_->prev_last_visible_char = '\0';
    }

    const char first_vis = FirstVisibleChar(wrapped);
    if (stripped_leading_nl && impl_->current_width > 0 &&
        first_vis != '\0' && first_vis != ' ' && wrapped[0] != ' ' &&
        !IsHardBreakAt(wrapped, 0)) {
        wrapped.insert(0, kSingleByteLen, ' ');
    }
    if (!stripped_leading_nl && impl_->prev_ended_with_inline_wait &&
        impl_->current_width > 0 && first_vis != '\0' && first_vis != ' ' &&
        wrapped[0] != ' ' && !IsHardBreakAt(wrapped, 0)) {
        if (IsClauseOrSentencePunctuation(impl_->prev_last_visible_char) &&
            IsAsciiAlphaNum(first_vis)) {
            wrapped.insert(0, kSingleByteLen, ' ');
        }
    }

    const std::size_t start_width = impl_->current_width;
    impl_->current_width = WordWrapByWidth(
        wrapped, impl_->config.max_line_width, start_width, *impl_->measurer);

    impl_->prev_ended_with_inline_wait = EndsWithInlineWaitTag(wrapped);
    impl_->prev_last_visible_char = NextPrevLastVisibleChar(
        impl_->current_width, impl_->prev_last_visible_char, first_vis,
        wrapped);

    impl_->last_dialogue_src_key = key;
    impl_->last_dialogue_wrapped = wrapped;
    impl_->last_start_width = start_width;
    impl_->last_is_continuation = is_continuation;

    const std::size_t keep = Utf8SafeTruncateForWideLimit(
        wrapped, impl_->config.max_output_bytes, max_wchars);
    result.translated = true;
    result.is_continuation = is_continuation;
    result.start_width = start_width;
    result.end_width = impl_->current_width;
    result.truncated = keep < wrapped.size();
    result.text.assign(wrapped.data(), keep);
    StripDanglingTrailingBackslash(result.text);
    return result;
}

struct TranslationSlotRingBuffer::Impl {
    std::array<std::array<char, kDefaultSlotByteCapacity>, kDefaultSlotPoolCount>
        pool{};
    std::size_t pool_index = 0;
    const char* last_dialogue_slot = nullptr;
    std::size_t last_dialogue_slot_len = 0;
};

TranslationSlotRingBuffer::TranslationSlotRingBuffer()
    : impl_(std::make_unique<Impl>()) {}

TranslationSlotRingBuffer::~TranslationSlotRingBuffer() = default;

TranslationSlotRingBuffer::TranslationSlotRingBuffer(
    TranslationSlotRingBuffer&&) noexcept = default;

TranslationSlotRingBuffer& TranslationSlotRingBuffer::operator=(
    TranslationSlotRingBuffer&&) noexcept = default;

void TranslationSlotRingBuffer::Reset() noexcept {
    impl_->pool_index = 0;
    impl_->last_dialogue_slot = nullptr;
    impl_->last_dialogue_slot_len = 0;
}

char* TranslationSlotRingBuffer::AllocateSlot() noexcept {
    char* const slot = impl_->pool[impl_->pool_index].data();
    impl_->pool_index = (impl_->pool_index + kSingleByteLen) % kDefaultSlotPoolCount;
    if (slot == impl_->last_dialogue_slot) {
        impl_->last_dialogue_slot = nullptr;
        impl_->last_dialogue_slot_len = 0;
    }
    return slot;
}

const char* TranslationSlotRingBuffer::Store(
    std::string_view text,
    bool cache_as_dialogue,
    std::size_t* out_copied_len) noexcept {
    char* const slot = AllocateSlot();
    const std::size_t copy_len =
        std::min(text.size(), kDefaultSlotByteCapacity - kSingleByteLen);
    if (copy_len > 0) {
        std::memcpy(slot, text.data(), copy_len);
    }
    slot[copy_len] = '\0';
    if (cache_as_dialogue) {
        impl_->last_dialogue_slot = slot;
        impl_->last_dialogue_slot_len = copy_len;
    }
    if (out_copied_len != nullptr) {
        *out_copied_len = copy_len;
    }
    return slot;
}

const char* TranslationSlotRingBuffer::FindReusableDialogueSlot(
    std::size_t expected_len) const noexcept {
    if (impl_->last_dialogue_slot == nullptr ||
        impl_->last_dialogue_slot_len != expected_len) {
        return nullptr;
    }
    return impl_->last_dialogue_slot;
}

int ParseTsvBuffer(const char* data, std::size_t len, TranslationMap& out) {
    if (data == nullptr || len == 0) {
        return 0;
    }

    std::size_t start = 0;
    if (len >= kUtf8BomLen &&
        static_cast<unsigned char>(data[kBomIndex0]) == kUtf8Bom0 &&
        static_cast<unsigned char>(data[kBomIndex1]) == kUtf8Bom1 &&
        static_cast<unsigned char>(data[kBomIndex2]) == kUtf8Bom2) {
        start = kUtf8BomLen;
    }

    int count = 0;
    std::size_t i = start;
    while (i < len) {
        const std::size_t line_start = i;
        while (i < len && data[i] != '\n' && data[i] != '\r') {
            ++i;
        }
        const std::size_t line_end = i;
        while (i < len && (data[i] == '\r' || data[i] == '\n')) {
            ++i;
        }

        std::size_t tab = line_start;
        while (tab < line_end && data[tab] != '\t') {
            ++tab;
        }
        if (tab == line_start || tab >= line_end) {
            continue;
        }

        std::string jp(data + line_start, tab - line_start);
        std::string en(data + tab + kSingleByteLen,
                       line_end - (tab + kSingleByteLen));
        UnescapeInPlace(jp);
        UnescapeInPlace(en);
        if (jp.empty() || en.empty()) {
            continue;
        }
        out[jp] = en;
        ++count;
    }
    return count;
}

bool LoadTsvFile(const std::string& path, TranslationMap& out) {
    if (path.empty()) {
        return false;
    }
    try {
        std::ifstream in(std::filesystem::u8path(path),
                         std::ios::binary | std::ios::ate);
        if (!in) {
            return false;
        }
        const std::streamsize sz = in.tellg();
        if (sz <= 0) {
            return false;
        }
        in.seekg(0, std::ios::beg);
        std::vector<char> buf(static_cast<std::size_t>(sz));
        if (!in.read(buf.data(), sz)) {
            return false;
        }
        return ParseTsvBuffer(buf.data(), buf.size(), out) > 0;
    } catch (const std::exception&) {
        return false;
    }
}

const std::string* FindTranslation(const TranslationMap& map,
                                   const std::string& src) {
    const auto it = map.find(src);
    return (it != map.end()) ? &it->second : nullptr;
}

}  // namespace translator_logic
