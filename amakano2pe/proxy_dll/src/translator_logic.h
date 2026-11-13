// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// translator_logic.h  (amakano2pe / CatSystem2)
//
// Pure OS-independent logic that powers the amakano2pe runtime translator.
// Everything here is free of Win32 / GDI / hook / trampoline dependencies so
// the same object code links into both the proxy DLL and GoogleTest binaries.

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace translator_logic {

using TranslationMap = std::unordered_map<std::string, std::string>;
using SpeakerKeySet = std::unordered_set<std::string>;

// Pluggable text-span width measurement strategy used by WordWrapByWidth
// and DialogueTranslationSession.
// Thread-safe: implementations must allow concurrent MeasureSpanWidth calls.
class TextWidthMeasurer {
public:
    TextWidthMeasurer() = default;
    TextWidthMeasurer(const TextWidthMeasurer&) = default;
    TextWidthMeasurer& operator=(const TextWidthMeasurer&) = default;
    TextWidthMeasurer(TextWidthMeasurer&&) noexcept = default;
    TextWidthMeasurer& operator=(TextWidthMeasurer&&) noexcept = default;
    virtual ~TextWidthMeasurer();

    virtual std::size_t MeasureSpanWidth(std::string_view utf8_span) const = 0;
};

// Counts 1 width unit per UTF-8 byte (matches WordWrapForEngine's contract).
// Thread-safe.
class MonospaceByteWidthMeasurer final : public TextWidthMeasurer {
public:
    ~MonospaceByteWidthMeasurer() override;
    std::size_t MeasureSpanWidth(std::string_view utf8_span) const override;
};

// Counts a fixed width per decoded UTF-8 scalar or replacement byte.
// Thread-safe.
class CodePointWidthMeasurer final : public TextWidthMeasurer {
public:
    explicit CodePointWidthMeasurer(std::size_t width_per_codepoint = 1) noexcept;
    ~CodePointWidthMeasurer() override;
    std::size_t MeasureSpanWidth(std::string_view utf8_span) const override;

private:
    std::size_t width_per_codepoint_;
};

// Undo the TSV escape sequences: \r \n \t \\. In-place.
void UnescapeInPlace(std::string& s);

// Truncate the NUL-terminated UTF-8 string `s` to at most `max_bytes`
// bytes on a valid UTF-8 scalar boundary without reading past `'\0'`.
std::size_t Utf8SafeTruncate(const char* s, std::size_t max_bytes);

// Truncate `utf8` so that its byte length is <= `max_bytes` and its UTF-16
// wchar_t length under MultiByteToWideChar(CP_UTF8, 0, ...) (excluding NUL)
// fits within `max_wchars - 1` when `max_wchars > 0` (when `max_wchars == 0`,
// only `max_bytes` constrains the result).
std::size_t Utf8SafeTruncateForWideLimit(std::string_view utf8,
                                         std::size_t max_bytes,
                                         std::size_t max_wchars);

// If `s` begins with the 2-byte sequence `\n` or `\N` (CatSystem2 textbox
// line-break control code -- NOT an ASCII 0x0A), replace it with a single
// space and return true.
bool StripLeadingControlNewline(std::string& s);

// Returns true iff `s` ends with an unescaped CatSystem2 inline keywait
// control tag `\@` (an odd run of backslashes followed by '@').
bool EndsWithInlineWaitTag(std::string_view s) noexcept;

// Returns true iff `s` ends with an odd run of `\` bytes (a dangling unescaped
// trailing backslash that would escape the leading `\` of an appended tag).
bool HasDanglingTrailingBackslash(std::string_view s) noexcept;

// Strips a single trailing `\` when `HasDanglingTrailingBackslash(s)` holds,
// preserving valid escaped literal backslash pairs (`"\\\\"`).
void StripDanglingTrailingBackslash(std::string& s) noexcept;

// Pre-wrap English text at word boundaries by inserting 2-byte `\n`
// CatSystem2 line-break control codes before any word whose measured width
// pushes the current line past `max_width`.
//
// CatSystem2 zero-width inline control escapes (`\@`, `\p`/`\P`, `\f`/`\F`,
// `\w`/`\W`, `\c`/`\C`, `\x`/`\X`, `\y`/`\Y`, `\i`/`\I`, `\r`/`\R`, `\t`/`\T`,
// `\z`/`\Z`, `\$`) contribute 0 visual width, `\s`/`\S` measures as fullwidth
// space U+3000 while keeping its raw escape intact, and escapes are never
// split mid-sequence. Returns the running line width at the end of `s`.
std::size_t WordWrapByWidth(std::string& s, std::size_t max_width,
                            std::size_t start_width,
                            const TextWidthMeasurer& measurer);

// Byte-count wrapper delegating to WordWrapByWidth with MonospaceByteWidthMeasurer.
std::size_t WordWrapForEngine(std::string& s, std::size_t line_chars,
                              std::size_t start_col = 0);

// Returns the complete set of UTF-8 speaker-name (`NAME`) lookup keys present
// in `translation_table.tsv` (zero overlap with `MESSAGE` keys).
const SpeakerKeySet& DefaultSpeakerNameKeys();

inline constexpr std::size_t kDefaultMaxDialogueWidthPx = 680;
inline constexpr std::size_t kSingleCharDecoderMaxWchars = 16;
inline constexpr std::size_t kMaxSpeakerWideChars = 4096;
inline constexpr std::size_t kDefaultMaxOutputBytes = 4095;
inline constexpr std::size_t kDefaultSlotPoolCount = 64;
inline constexpr std::size_t kDefaultSlotByteCapacity = 4096;

struct DialogueSessionConfig {
    std::size_t max_line_width = kDefaultMaxDialogueWidthPx;
    std::size_t single_char_passthru_wchars = kSingleCharDecoderMaxWchars;
    std::size_t max_speaker_wchars = kMaxSpeakerWideChars;
    std::size_t max_output_bytes = kDefaultMaxOutputBytes;
};

struct TranslationResult {
    bool translated = false;
    bool deduplicated = false;
    bool is_speaker_name = false;
    bool is_continuation = false;
    bool truncated = false;
    std::size_t start_width = 0;
    std::size_t end_width = 0;
    std::string text;
};

// Encapsulates stateful CatSystem2 runtime lookup, speaker-name isolation,
// single-char glyph-decoder passthrough, layout/draw pass deduplication,
// cross-MESSAGE `\n` and `\@` width carry, and safe output truncation.
// Thread-compatible: callers must synchronize concurrent access.
class DialogueTranslationSession {
public:
    DialogueTranslationSession(const TranslationMap* translations,
                               const TextWidthMeasurer* measurer,
                               DialogueSessionConfig config = {});
    ~DialogueTranslationSession();

    DialogueTranslationSession(const DialogueTranslationSession&) = delete;
    DialogueTranslationSession& operator=(const DialogueTranslationSession&) = delete;
    DialogueTranslationSession(DialogueTranslationSession&&) noexcept;
    DialogueTranslationSession& operator=(DialogueTranslationSession&&) noexcept;

    void SetMaxLineWidth(std::size_t max_line_width) noexcept;
    void SetSpeakerNameKeys(SpeakerKeySet speaker_name_keys);
    std::size_t MaxLineWidth() const noexcept;
    std::size_t CurrentLineWidth() const noexcept;
    bool PrevEndedWithInlineWait() const noexcept;
    void ResetDialogueState() noexcept;

    TranslationResult Translate(std::string_view src, std::size_t max_wchars);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Fixed-capacity ring buffer of NUL-terminated translation output buffers that
// tracks the most recent dialogue slot for layout/draw deduplication and
// invalidates that cached pointer whenever ring-buffer wraparound reclaims it.
// Thread-compatible: callers must synchronize concurrent access.
class TranslationSlotRingBuffer {
public:
    TranslationSlotRingBuffer();
    ~TranslationSlotRingBuffer();

    TranslationSlotRingBuffer(const TranslationSlotRingBuffer&) = delete;
    TranslationSlotRingBuffer& operator=(const TranslationSlotRingBuffer&) = delete;
    TranslationSlotRingBuffer(TranslationSlotRingBuffer&&) noexcept;
    TranslationSlotRingBuffer& operator=(TranslationSlotRingBuffer&&) noexcept;

    void Reset() noexcept;
    char* AllocateSlot() noexcept;
    const char* Store(std::string_view text,
                      bool cache_as_dialogue,
                      std::size_t* out_copied_len = nullptr) noexcept;
    const char* FindReusableDialogueSlot(std::size_t expected_len) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

int ParseTsvBuffer(const char* data, std::size_t len, TranslationMap& out);
bool LoadTsvFile(const std::string& path, TranslationMap& out);
const std::string* FindTranslation(const TranslationMap& map,
                                   const std::string& src);

}  // namespace translator_logic
