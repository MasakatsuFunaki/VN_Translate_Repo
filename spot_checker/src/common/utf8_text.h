// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace spot_checker {

inline constexpr char32_t kUnicodeReplacementChar = 0xFFFDU;
inline constexpr char32_t kMaxUnicodeCodepoint = 0x10FFFFU;

// Decodes one Unicode scalar value at `text[index]` per RFC 3629 and advances
// `index` past the consumed bytes.
//
// Preconditions / Boundary behavior:
// - If `index >= text.size()`, leaves `index` unchanged and returns U'\0'.
// - Valid 1-byte (`0x00..0x7F`), 2-byte (`0xC2..0xDF` + 1 continuation),
//   3-byte (excluding overlong `0xE0 0x80..0x9F` and UTF-16 surrogates
//   `U+D800..U+DFFF`), and 4-byte (`U+10000..U+10FFFF`, excluding overlong
//   `0xF0 0x80..0x8F` and `> U+10FFFF`) sequences return the decoded scalar
//   value and advance `index` by 1..4 bytes.
// - On any ill-formed sequence (lone continuation `0x80..0xBF`, overlong lead
//   `0xC0..0xC1`, invalid lead `0xF5..0xFF`, UTF-16 surrogate lead, or
//   truncated multi-byte sequence followed by end-of-string or a non-continuation
//   byte such as ASCII '"' or '%'), advances `index` by the RFC 3629 maximal
//   valid subpart (never consuming the following valid starter byte) and
//   returns `kUnicodeReplacementChar` (`U'\uFFFD'`).
char32_t utf8_next(std::string_view text, std::size_t& index) noexcept;

// Counts Unicode scalar / replacement units produced by `utf8_next` across `text`.
std::size_t utf8_codepoint_count(std::string_view text) noexcept;

// Appends the RFC 3629 UTF-8 encoding of `cp` to `out`. If `cp > kMaxUnicodeCodepoint`
// or `cp` is a UTF-16 surrogate (`0xD800..0xDFFF`), appends `kUnicodeReplacementChar`.
void append_utf8(std::string& out, char32_t cp);

// Converts UTF-16 (`wchar_t` on Windows) to RFC 3629 UTF-8. Valid surrogate
// pairs encode as 4-byte astral scalars; unpaired high or low surrogates
// encode as `kUnicodeReplacementChar` without consuming an adjacent non-surrogate.
std::string wide_to_utf8(std::wstring_view wide);

// Converts UTF-8 to UTF-16 (`std::wstring`), encoding astral scalars as
// surrogate pairs and ill-formed UTF-8 subparts as `U'\uFFFD'`.
std::wstring utf8_to_wide(std::string_view utf8);

// Script classification predicates on decoded Unicode scalar values:
// - `is_hiragana`: U+3040..U+309F.
// - `is_katakana`: fullwidth U+30A0..U+30FF, phonetic extensions U+31F0..U+31FF,
//   and halfwidth katakana U+FF66..U+FF9F.
// - `is_lexical_katakana`: `is_katakana(cp)` excluding non-lexical interpunct /
//   prolongation marks U+30A0 ('゠'), U+30FB ('・'), U+30FC ('ー'), and U+FF70 ('ｰ').
// - `is_kanji`: CJK Unified Ideographs (U+4E00..U+9FFF), Extension A
//   (U+3400..U+4DBF), Extension B (U+20000..U+2A6DF), Compatibility Ideographs
//   (U+F900..U+FAFF), and the ideographic iteration mark U+3005 ('々').
bool is_hiragana(char32_t cp) noexcept;
bool is_katakana(char32_t cp) noexcept;
bool is_lexical_katakana(char32_t cp) noexcept;
bool is_kanji(char32_t cp) noexcept;

// Returns true if `text` contains any Japanese kana, kanji (including '々'),
// CJK symbol/punctuation (U+3000..U+303F, U+FF61..U+FF65), or fullwidth/halfwidth
// katakana/ASCII form (U+FF01..U+FF9F).
bool has_japanese(std::string_view text) noexcept;

// Returns true if `text` contains at least one lexical Japanese character
// (`is_hiragana(cp) || is_lexical_katakana(cp) || is_kanji(cp)`).
// Pure punctuation/silence lines such as "「……」", "「・・・」", or "「ー」" return false,
// whereas lines containing kanji compounds ("人々") or halfwidth katakana ("ｶﾞﾀｯ")
// return true.
bool has_real_japanese(std::string_view text) noexcept;

// Returns true when `!has_real_japanese(text)` and `text` contains no ASCII
// alphanumeric byte ('A'..'Z', 'a'..'z', '0'..'9').
bool is_pure_silence_or_punctuation(std::string_view text) noexcept;

// Returns true if `text` contains leaked CJK or fullwidth/typographic punctuation
// forbidden in sanitized English prose: U+3000..U+303F ('　', '、', '。', '「', '」',
// '『', '』', '【', '】', '〜'), U+2014 ('—'), U+2015 ('―'), U+2025 ('‥'),
// U+2026 ('…'), U+2500 ('─'), or U+FF01..U+FF65 ('！', '？', '（', '）', '～', etc.).
bool has_cjk_or_fullwidth_punctuation(std::string_view text) noexcept;

// ASCII whitespace (' ', '\t', '\r', '\n') trimming helpers.
std::string_view trim_ascii_view(std::string_view text) noexcept;
std::string trim_ascii(std::string_view text);
std::string trim_left_ascii(std::string_view text);
std::string trim_right_ascii(std::string_view text);

// Trims ASCII whitespace, U+3000 IDEOGRAPHIC SPACE ('　'), and U+FEFF BOM from
// both ends of `text`.
std::string trim_unicode(std::string_view text);

// Lowercases ASCII 'A'..'Z' -> 'a'..'z', leaving all other bytes (including
// UTF-8 continuation/lead bytes >= 0x80) unchanged.
std::string to_lower_ascii(std::string_view text);

// Returns true for ASCII alphanumeric ('a'..'z', 'A'..'Z', '0'..'9') and any
// non-ASCII UTF-8 byte (>= 0x80), preventing false word boundaries inside
// accented or macronized words (e.g. "tōi", "cliché").
bool is_word_byte(unsigned char b) noexcept;

// Case-insensitive whole-word search for `needle` inside `haystack`, bounded on
// both sides by start/end-of-string or `!is_word_byte(...)`. Returns false when
// `needle` is empty.
bool contains_word_ascii_ci(std::string_view haystack,
                            std::string_view needle) noexcept;

// Normalizes typographic dashes, ellipses, smart quotes, wave dashes, and
// ideographic spaces to plain ASCII ("—"/"–"/"―"/"─" -> "--", "…"/"‥" -> "...",
// "‘"/"’" -> "'", "“"/"”" -> "\"", "〜"/"～" -> "~", "　" -> " ").
std::string sanitize_ascii_punctuation(std::string_view text);

}  // namespace spot_checker
