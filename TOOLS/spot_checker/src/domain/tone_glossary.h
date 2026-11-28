// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace spot_checker {

inline constexpr std::size_t kMinGlossaryJpCodepoints = 2;
inline constexpr std::size_t kMinGlossaryEnBytes = 3;

struct GlossaryEntry {
    std::string jp_term;
    std::string en_term;
    bool is_person_name = false;
};

// Game-agnostic tone guide & canonical glossary value object.
// Supports games with `TONE_OF_NOVEL.md` / `TONE.md` as well as C++ pipelines
// without markdown guides by harvesting supplemental `(jp_speaker -> en_speaker)`
// pairs directly from `translated_text.json`.
class ToneGlossary {
public:
    ToneGlossary() = default;

    // Parses `<name_reference>`, `<character_dossier>`, `<terminology>`, and
    // Markdown `| JP | EN |` / `JP -> EN` / `JP = EN` tables from `raw_tone_md`.
    // Strips `<formatting_rules>...</formatting_rules>` and batch-translator
    // formatting boilerplates from `sanitized_guide_markdown()`.
    // Terms with `< kMinGlossaryJpCodepoints` Japanese codepoints or
    // `< kMinGlossaryEnBytes` ASCII bytes are excluded from `entries()` to
    // prevent single-kanji/short-syllable false positives, while all valid
    // speaker pairs remain in `speaker_map()`.
    static ToneGlossary parse_markdown(std::string_view raw_tone_md);

    static std::string strip_translator_formatting_rules(
        std::string_view raw_tone_md);

    // Registers a supplemental `(jp_speaker -> en_speaker)` pair (e.g. from
    // `"speaker"` + `"speaker_en"` or `type == "NAME"` script entries) without
    // overwriting an existing canonical mapping from `TONE_OF_NOVEL.md`.
    // Also adds a `GlossaryEntry{..., is_person_name = true}` if length
    // thresholds (`kMinGlossaryJpCodepoints`, `kMinGlossaryEnBytes`) are met.
    void merge_speaker_pair(std::string_view jp_speaker,
                            std::string_view en_speaker);

    // Adds a glossary entry directly (no-op if `jp_term` < 2 JP codepoints or
    // `en_term` < 3 bytes or duplicate `jp_term`).
    void add_term(std::string_view jp_term,
                  std::string_view en_term,
                  bool is_person_name);

    // Resolves `raw_speaker`: empty/whitespace -> `"NARRATION"`, known JP name
    // -> canonical English name, otherwise trimmed `raw_speaker`.
    std::string resolve_speaker(std::string_view raw_speaker) const;

    const std::string& sanitized_guide_markdown() const noexcept {
        return sanitized_guide_md_;
    }

    const std::vector<GlossaryEntry>& entries() const noexcept {
        return entries_;
    }

    const std::unordered_map<std::string, std::string>& speaker_map() const noexcept {
        return speaker_map_;
    }

    // Enforces canonical glossary preservation between `old_en_body` and
    // `new_en_body` for every `GlossaryEntry` whose `jp_term` occurs in `jp_body`
    // and whose `en_term` occurred (whole-word, case-insensitively) in `old_en_body`:
    // - Non-person terms (`!is_person_name`: places, schools, lore, items) must
    //   ALWAYS remain present in `new_en_body` (pronouns never excuse dropping them).
    // - Person names (`is_person_name == true`) may be omitted ONLY when the
    //   count of whole-word 3rd-person English pronouns (`he`, `him`, `his`,
    //   `she`, `her`, `hers`, `they`, `them`, `their`, `theirs`) in `new_en_body`
    //   strictly increases over `old_en_body` (1st/2nd-person `"I"` / `"you"`
    //   never disable glossary enforcement).
    // On failure, writes `"jp_term -> en_term"` to `*violated_term_out` (if
    // non-null) and returns false.
    bool check_glossary_preservation(std::string_view jp_body,
                                     std::string_view old_en_body,
                                     std::string_view new_en_body,
                                     std::string* violated_term_out = nullptr) const;

private:
    std::string sanitized_guide_md_;
    std::vector<GlossaryEntry> entries_;
    std::unordered_map<std::string, std::string> speaker_map_;
};

}  // namespace spot_checker
