// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once

#include "common/status.h"
#include "domain/spot_check_types.h"
#include "domain/tone_glossary.h"
#include "ports/crypt_sync.h"
#include "ports/file_store.h"
#include "storage/json_format_style.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace spot_checker {

struct LoadedCorpus {
    std::vector<ScriptEntry> entries;
    ToneGlossary glossary;
    JsonFormatStyle translated_json_style;
};

struct SyncCounts {
    std::size_t script_entries_updated = 0;
    std::size_t cache_entries_updated = 0;
};

// Game-agnostic repository for `script_output/translated_text.json`,
// `script_output/translation_cache_*.json`, optional `TONE_OF_NOVEL.md` /
// `TONE.md`, and `VNE1`/`VNE2` `.json.enc` synchronization.
//
// Supported `translated_text.json` container shapes across all engines:
// 1. `{ "<script>": { "strings": [ ... ] } }`
// 2. `{ "<script>": { "lines": [ ... ] } }`
// 3. Optional top-level `"scripts": { ... }` wrapper and/or direct array
//    `{ "<script>": [ ... ] }`.
//
// Supported entry keys:
// - Source JP: `"text"` -> `"content"` -> `"jp"`.
// - Target EN: `"translated"` -> `"translation"` -> `"en"`.
// - Speaker attribution: explicit `"speaker"` / `"speaker_en"` (`null`/`""` ->
//   `"NARRATION"`), stateful preceding `type == "NAME"` / `"name"` entries
//   (cleared on `type == "COMMAND"` speaker-reset rows), and inline
//   `Speaker\r\nBody` prefixes.
class ScriptRepository {
public:
    ScriptRepository(IFileStore& store, ICryptSync& crypt) noexcept;

    // Pure in-memory corpus parser for hermetic unit testing and disk loading.
    // Returns `ErrorCode::CorruptJson` on invalid JSON or non-object root.
    static Result<LoadedCorpus> parse_corpus_memory(
        std::string_view translated_json_utf8,
        std::string_view tone_markdown_utf8 = {});

    // Pure in-memory updater applying `improvements` to `translated_json_utf8`:
    // - Updates `(script_name, entry_index)` target rows AND cross-script
    //   duplicate `jp_text` rows whose current translation equals `old_en` or
    //   untranslated `jp_text`.
    // - Writes `"translated"` and mirrors into `"translation"` / `"en"` whenever
    //   those keys already exist on the entry object.
    // - Preserves `detect_json_format_style(translated_json_utf8)` (indent,
    //   LF/CRLF, trailing newline).
    // Returns `ErrorCode::CorruptJson` or `ErrorCode::InvalidArgument` on
    // malformed JSON or out-of-range target index.
    static Result<std::string> apply_improvements_memory(
        std::string_view translated_json_utf8,
        const std::vector<Improvement>& improvements,
        std::size_t* out_script_entries_updated = nullptr);

    // Decrypts if needed, verifies `<game_root>/script_output/translated_text.json`
    // exists (returning `ErrorCode::IoError` / `ErrorCode::CryptSyncFailed` /
    // `ErrorCode::CorruptJson` on failure), loads optional `TONE_OF_NOVEL.md` /
    // `TONE.md`, and parses `LoadedCorpus`.
    Result<LoadedCorpus> load_from_game_dir(
        const std::filesystem::path& game_root) const;

    // Applies `improvements` atomically to `translated_text.json` and every
    // `translation_cache_*.json` discovered in `<game_root>/script_output/`
    // (updating existing `jp_text` keys and stripped-inline-control keys whose
    // prior value matched `old_en`, preserving insertion order and formatting
    // style), then invokes `crypt_.encrypt_if_tracked(game_root)`.
    Result<SyncCounts> apply_improvements_to_artifacts(
        const std::filesystem::path& game_root,
        const std::vector<Improvement>& improvements) const;

private:
    IFileStore& store_;
    ICryptSync& crypt_;
};

}  // namespace spot_checker
