// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// TSV parsing, trailing-CRLF normalized translation lookup, full-string CP932
// `#` color-tag sanitization, built-in runtime UI fallback seeding, single-line
// UI font adjustment, and FXF windowTitle decoding for Mushigurui.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace translator_logic {

using TranslationMap = std::unordered_map<std::string, std::string>;

constexpr int kMinUiUnpatchedFontSize = 20;
constexpr std::size_t kMediumChoiceCharLimit = 48;
constexpr int kStandardChoiceFontSize = 16;
constexpr int kCompactChoiceFontSize = 14;
constexpr int kPatchedChoiceSpace = -4;
constexpr int kVerticalCenteringDivisor = 2;

constexpr std::size_t kRuntimeUiFallbackCount = 22;

struct UiFontAdjustment {
    bool shouldAdjust = false;
    int newFontSize = 0;
    int newSpace = 0;
    int yOffset = 0;
    bool expandMaxChars = false;
};

struct RuntimeUiFallbackEntry {
    const wchar_t* jp_text = L"";
    const char* en_text = "";
};

const std::array<RuntimeUiFallbackEntry, kRuntimeUiFallbackCount>&
GetRuntimeUiFallbackEntries() noexcept;

// Encode a UTF-16 string to Shift-JIS (CP932) without best-fit substitution.
// Returns false if any character cannot be represented losslessly in CP932.
bool EncodeWideToCp932(std::wstring_view wide_text, std::string& out_cp932);

// Decode TSV escape sequences (`\r`, `\n`, `\t`, `\\`) in place while stepping
// valid 2-byte CP932 characters first so `0x5C` (`'\\'`) trail bytes (such as
// `―`, `能`, `表`) are preserved intact.
void UnescapeInPlace(std::string& s);

std::size_t Utf8BomLen(const char* data, std::size_t len);

// Parse a TSV buffer in memory ("jp\ten\n" or "\r\n") into `out` without
// stripping `#` inside keys or values. Returns the number of rows parsed.
int ParseTsvBuffer(const char* data, std::size_t len, TranslationMap& out);

// Return a copy of `s` with all trailing '\r' and '\n' bytes removed while
// preserving internal '\r\n' line breaks verbatim.
std::string StripTrailingCrLf(std::string_view s);

void FlattenNewlinesInPlace(std::string& s);

bool HasCp932ColorPrefix(const char* s) noexcept;

// Scan a CP932 string left-to-right: preserve valid 3-byte `#XX` CP932 color
// tags, strip line-leading markdown `# ` prefixes (including after `%`, `#XX`,
// or `\r\n`), and replace any mid-line or trailing ASCII `#` (`0x23`) with
// 2-byte fullwidth CP932 `＃` so TextRenderCore never consumes 3 bytes or
// overruns `\0`.
void SanitizeAsciiHashInPlace(std::string& s);

bool LookupTranslation(const TranslationMap& map,
                       const char* originalText,
                       bool singleLineMode,
                       std::string& outTranslated);

bool HasAsciiWords(const char* text) noexcept;

std::size_t VisibleAsciiLen(const char* text) noexcept;

UiFontAdjustment ComputeUiFontAdjustment(const char* text,
                                         bool wasTranslated,
                                         int curFontSize,
                                         int curSpace,
                                         int curMaxChars) noexcept;

std::string ExtractFxfWindowTitle(const std::uint8_t* fxfBytes, std::size_t len);

// Dual-indexed translation table providing O(1) exact lookup (HD10 strings
// ending in "\r\n") and O(1) trailing-CRLF-normalized lookup (Complete Edition
// strings without trailing "\r\n"), plus non-overwriting seeding of the 22
// built-in Music Gallery and Config Page 2 runtime UI strings.
class TranslationTable {
public:
    TranslationTable() = default;

    void Clear() noexcept;

    // Parse TSV buffer (automatically skipping any UTF-8 BOM) and index all
    // entries into both exact and trailing-CRLF-normalized maps.
    int LoadFromTsvBuffer(const char* data, std::size_t len);

    // Insert or overwrite a single JP -> EN entry in both maps.
    void InsertOrAssign(std::string jp, std::string en);

    // Seed all 22 built-in runtime UI fallback strings using non-overwriting
    // insertion so existing TSV entries always take precedence. Returns the
    // number of fallback entries newly inserted into the exact map.
    std::size_t SeedRuntimeUiFallbacks();

    bool Lookup(const char* originalText,
                bool singleLineMode,
                std::string& outTranslated) const;

    std::size_t ExactSize() const noexcept { return exact_.size(); }
    std::size_t StrippedSize() const noexcept { return stripped_.size(); }
    bool Empty() const noexcept { return exact_.empty() && stripped_.empty(); }

private:
    const std::string* FindCandidate(std::string_view key) const;

    TranslationMap exact_;
    TranslationMap stripped_;
};

}  // namespace translator_logic
