// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once

// OS-independent UTF-16 runtime translation table, bypass rules, NVL word-wrap,
// and session facade for SiglusEngine.exe. Free of Windows headers so unit and
// component tests exercise the exact object code shipped in winmm.dll.

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace translator_logic {

inline constexpr std::size_t kDefaultNarrationCols = 56u;
inline constexpr std::size_t kDefaultDialogueCols = 50u;

struct NvlWrapConfig {
    std::size_t narration_cols = kDefaultNarrationCols;
    std::size_t dialogue_cols = kDefaultDialogueCols;
    bool is_choice = false;
};

// Strict UTF-8 <-> UTF-16 conversions without OS locale dependencies.
// Rejects overlong sequences, truncated multi-byte sequences, and unpaired
// surrogates, returning false and clearing `out` on malformed input.
bool Utf8ToUtf16(std::string_view utf8, std::wstring& out);
bool Utf16ToUtf8(std::wstring_view utf16, std::string& out);

// Decodes TSV escape sequences (\r, \n, \t, \\) in place. Unknown escape pairs
// (such as \q) and trailing single backslashes remain intact.
void UnescapeInPlace(std::string& s);
void UnescapeInPlace(std::wstring& s);

// Returns true when a runtime engine string must never be replaced:
//   1. Empty string
//   2. Every code unit is ASCII (< 0x0080), preserving scene/asset tokens
//   3. SiglusEngine protagonist #NAMAE macro keys L"＊Ａ" or L"＊Ｂ"
bool ShouldBypassTranslation(std::wstring_view s) noexcept;

// Returns true when `text` (after skipping leading whitespace) starts with an
// opening dialogue quote (", ', “, ‘, 「, 『).
bool IsDialogueLine(std::wstring_view text) noexcept;

// Wraps English prose for Adieu's 640x480 full-screen NVL window (#MWND.002)
// using real L'\n' (U+000A) line breaks at ASCII word boundaries.
// Preserves existing explicit L'\n' breaks, leaves choice strings (`is_choice`)
// and lines within the active column budget unwrapped, and splits pathological
// unbroken tokens (> active column budget) cleanly with zero trailing spaces.
std::wstring WrapForSiglusNvl(std::wstring_view en, NvlWrapConfig cfg = {});

// Thread-compatible UTF-16 Japanese -> English lookup table insulated via PIMPL.
class TranslationTable {
public:
    using EntryVisitor =
        std::function<void(std::wstring_view jp, std::wstring_view en)>;

    TranslationTable();
    TranslationTable(const TranslationTable& other);
    TranslationTable(TranslationTable&& other) noexcept;
    TranslationTable& operator=(const TranslationTable& other);
    TranslationTable& operator=(TranslationTable&& other) noexcept;
    ~TranslationTable();

    // Parses a UTF-8 TSV buffer ("<jp>\t<en>\n" or "\r\n", optional UTF-8 BOM).
    // Unescapes both columns, skips malformed/empty/ill-formed-UTF-8 rows, and
    // inserts entries with first-wins duplicate-key semantics. Returns the
    // count of newly inserted keys.
    std::size_t ParseTsvBytes(std::string_view utf8_tsv);

    // Clears existing entries and loads a UTF-8 TSV file from `path`.
    // Returns true iff the file opened and yielded at least one valid entry.
    bool LoadFromFile(const std::filesystem::path& path);
    bool LoadFromUtf8Path(std::string_view utf8_path);

    void Clear() noexcept;

    const std::wstring* Find(std::wstring_view jp) const noexcept;
    bool Contains(std::wstring_view jp) const noexcept;
    std::size_t Size() const noexcept;
    bool Empty() const noexcept;

    void ForEach(const EntryVisitor& visitor) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct RuntimeStats {
    std::size_t total_calls = 0u;
    std::size_t bypassed_calls = 0u;
    std::size_t translated_hits = 0u;
    std::size_t lookup_misses = 0u;
};

// Public component facade orchestrating TSV loading, bypass filtering, table
// lookup, NVL word-wrapping, and post-hook buffer assignment.
// Thread-compatible: load once during startup before concurrent lookups, or
// synchronize external reloads.
class TranslationRuntimeFacade {
public:
    using AssignCallback = std::function<void(std::wstring_view replacement)>;

    TranslationRuntimeFacade();
    explicit TranslationRuntimeFacade(NvlWrapConfig wrap_config);
    TranslationRuntimeFacade(const TranslationRuntimeFacade& other);
    TranslationRuntimeFacade(TranslationRuntimeFacade&& other) noexcept;
    TranslationRuntimeFacade& operator=(const TranslationRuntimeFacade& other);
    TranslationRuntimeFacade& operator=(TranslationRuntimeFacade&& other) noexcept;
    ~TranslationRuntimeFacade();

    bool LoadTsvFile(const std::filesystem::path& path);
    bool LoadTsvUtf8Path(std::string_view utf8_path);
    std::size_t LoadTsvBuffer(std::string_view utf8_tsv);

    void SetWrapConfig(NvlWrapConfig cfg) noexcept;
    NvlWrapConfig WrapConfig() const noexcept;

    // Evaluates `current_str` produced by SiglusEngine's Scene::get_str or
    // Scene::get_cur_str. When eligible and present in the table, formats the
    // English string with WrapForSiglusNvl (or leaves unwrapped if `is_choice`)
    // and invokes `assign_fn` with the replacement. Returns true iff replaced.
    bool ApplyPostHook(std::wstring_view current_str,
                       const AssignCallback& assign_fn,
                       bool is_choice = false);

    std::size_t EntryCount() const noexcept;
    RuntimeStats Stats() const noexcept;
    void ResetStats() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace translator_logic
