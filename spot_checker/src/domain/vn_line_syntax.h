// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once

#include "domain/spot_check_types.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace spot_checker {

inline constexpr std::size_t kMaxSpeakerPrefixHeaderBytes = 48;
inline constexpr std::size_t kMinSustainedHSceneLines = 16;
inline constexpr std::size_t kMaxHSceneBridgeGapLines = 4;
inline constexpr std::size_t kMinSustainedHSceneActiveLines = 7;
inline constexpr std::size_t kMinSustainedHSceneAnchors = 3;
inline constexpr double kMinSustainedHSceneDensity = 0.35;

enum class HSceneCueTier {
    None = 0,
    AmbiguousBreath = 1,
    EroticVocalOrSfx = 2,
    ExplicitOrTagged = 3,
};

enum class OuterWrapKind {
    None = 0,
    AsciiDoubleQuote,
    CornerQuoteSingle,
    CornerQuoteDouble,
    AsciiParen,
    UnclosedLeadingAsciiQuote,
    UnclosedTrailingAsciiQuote,
};

struct QuoteEnvelope {
    std::string leading_escape;
    std::string trailing_escape;
    OuterWrapKind outer_kind = OuterWrapKind::None;
    std::size_t ascii_double_quote_count = 0;

    bool balanced_ascii_quotes() const noexcept {
        return (ascii_double_quote_count % 2U) == 0U;
    }
};

struct SpeakerSplit {
    std::string prefix;
    std::string speaker_header;
    std::string body;
};

// Splits `Speaker\r\nBody` or `Speaker\nBody` framing when `Speaker` is non-empty,
// `<= kMaxSpeakerPrefixHeaderBytes` bytes, contains no sentence-ending or quote
// punctuation ('.', '!', '?', '。', '！', '？', '"', '「', '」', '『', '』'),
// and `Body` is non-empty. Otherwise `prefix` and `speaker_header` are empty
// and `body == text`.
SpeakerSplit split_speaker_prefix(std::string_view text);

std::string extract_speaker_prefix(std::string_view text);
std::string strip_speaker_prefix_body(std::string_view text);

// Inspects the dialogue/narration body after separating leading literal "\n" /
// whitespace (`leading_escape`) and trailing literal "\@" (`trailing_escape`):
// - Detects closed outer `"..."`, `「...」`, `『...』`, and `(...)` envelopes.
// - Detects split-box open halves (`UnclosedLeadingAsciiQuote`, e.g. `"Since today,`)
//   and split-box close halves (`UnclosedTrailingAsciiQuote`, e.g. `we went home."`).
// - Counts all ASCII `"` characters in the core body.
QuoteEnvelope analyze_quote_envelope(std::string_view body);

// Verifies that `candidate_en_env` preserves the dialogue/thought/narration
// quote framing required by `jp_env` and `old_en_env`:
// - Preserves leading/trailing engine escapes (`leading_escape`, `trailing_escape`).
// - Preserves `OuterWrapKind` (including `AsciiParen` thoughts and split-box
//   `UnclosedLeadingAsciiQuote` / `UnclosedTrailingAsciiQuote` halves; if `old_en_env`
//   incorrectly dropped outer quotes present in `jp_env` (`CornerQuoteSingle` /
//   `CornerQuoteDouble`), allows restoring matching `AsciiDoubleQuote`).
// - Rejects unbalanced odd ASCII quote counts whenever `old_en_env` has balanced
//   quotes (or rejects changing parity on split-box single-quote halves).
bool quote_envelopes_compatible(const QuoteEnvelope& jp_env,
                                const QuoteEnvelope& old_en_env,
                                const QuoteEnvelope& candidate_en_env) noexcept;

// Dynamically extracts inline engine placeholders and control sequences from
// `text` in exact order of appearance (zero game-specific hardcoding):
// 1. `printf` format specifiers (`%[+-0 #]*[0-9]*(?:\.[0-9]+)?[sdifuxX%]`)
// 2. Engine string variables (`$str[0-9]+`)
// 3. Inline pause / control markers (`$&`, `<[-+]?[0-9]+>`)
// 4. Inline markup tags (`</?[A-Za-z!][^>\r\n]*>`, `[r]`, `[p]`, `[l]`, `[np]`)
// 5. Literal 2-char backslash engine escapes (`\n`, `\@`, `\p`, `\w`, `\c`, `\f`)
// 6. Real line-feed characters (`'\n'`, normalized from `\r\n` or bare `\n`).
std::vector<std::string> extract_engine_control_tokens(std::string_view text);

// Strips leading inline engine control prefixes (`<N>\`, `$&`) and ruby tags
// (`<"kanji|reading">` -> `kanji`, `[reading|kanji]` -> `kanji`) used by engine
// TSV key normalizers.
std::string strip_inline_control_wrappers(std::string_view text);

// Game-agnostic filter for untranslated engine asset, path, or sprite tokens:
// returns true if `!has_real_japanese(jp_text)` or if `jp_text == en_text` and
// trimmed `jp_text` starts with '_', contains '/', or contains both '@' and '_'
// with no whitespace.
bool is_engine_asset_or_command_token(std::string_view jp_text,
                                      std::string_view en_text) noexcept;

// Generic speaker H-tag suffix detector (never hardcodes character names):
// returns true when trimmed `speaker` ends with fullwidth 'Ｈ' (U+FF28),
// "（Ｈ）", or case-insensitive ASCII "(h)" / " h)".
bool has_speaker_h_tag_suffix(std::string_view speaker) noexcept;

// Returns true when `has_speaker_h_tag_suffix(speaker)` holds or `jp_body` /
// `en_body` matches unambiguous single-line explicit adult lexicon markers
// (whole-word matched in English). Used for single-line post-edit / unprompted
// explicit lexicon checks; corpus H-scene avoidance uses sustained span
// detection (`detect_h_scene_spans` / `compute_h_scene_line_mask`).
bool is_h_scene_entry(std::string_view speaker,
                      std::string_view jp_body,
                      std::string_view en_body) noexcept;

// Classifies a single stripped line (`speaker`, `jp_body`, `en_body`) into one
// of the 3 H-scene cue tiers used by sustained cluster detection:
// - `ExplicitOrTagged` (erotic anchor): `has_speaker_h_tag_suffix(speaker)`
//   holds, or `jp_body` / `en_body` contains explicit anatomical/sexual markers
//   or literary eroge markers (`秘裂`, `秘部`, `秘所`, `花弁`, `肉襞`, `粘膜`,
//   `蜜壺`, `白濁`, `抽挿`, `結合`, `愛液`, `愛撫`, `快楽`, `嬌声`, `shaft`,
//   `folds`, `wetness`, `member`, `groin`, `loins`, `climax`, `thrust`,
//   `thrusting`, `slick`, etc.).
// - `EroticVocalOrSfx` (erotic anchor, suppressed when combat/chase/horror
//   screams or weapon/injury lexicon appear without explicit/wet-SFX markers):
//   erotic heart symbols (`♡`, `♥`), wet/kissing/fluid SFX (`んちゅ`, `ちゅぱ`,
//   `じゅる`, `びゅる`, `ぐちゅ`, `ぬちゅ`, `くちゅ`, `ぴちゃ`, `ずぷ`, `slurp`,
//   `squelch`, `schlick`, `splurt`), nasal/pleasure moans (`んっ`, `あんっ`,
//   `んぅ`, `ふぁ`, `ひゃん`, `やぁん`, `イッちゃ`, `nngh`, `hnngh`, `mmph`,
//   `ngh`), repeated moan pairs, or elongated vocalizations (`aghhh+`, `kuhhh+`,
//   `haaah+`).
// - `AmbiguousBreath` (non-anchor density reinforcement, suppressed on
//   combat/pain/chase lines): standalone breath/gasp tokens (`はぁ`, `くぅ`,
//   `くっ`, `あっ`, `haah`, `kuh`, `aah`, `pant`, `gasp`).
// - `None`: normal dialogue, narration, or combat/pain/chase/horror lines
//   (`ぐあっ`, `うわあ`, `"Gah!"`, `"Huff, huff"`, blood/blade/wound/run cues).
HSceneCueTier classify_h_scene_cue(std::string_view speaker,
                                   std::string_view jp_body,
                                   std::string_view en_body) noexcept;

// Detects sustained multi-line H-scene spans (`{offset, length}` half-open
// intervals `[offset, offset + length)`) within each `script_name` segment of
// `entries`:
// - Bridges gaps of up to `kMaxHSceneBridgeGapLines` non-cue lines between cue
//   lines (`HSceneCueTier != None`) inside the same `script_name`, trimming
//   leading and trailing non-cue lines so `offset` and `offset + length - 1`
//   are always cue lines (zero bleed into adjacent normal dialogue).
// - Accepts a bridged segment iff `length >= kMinSustainedHSceneLines`,
//   active cue count `>= kMinSustainedHSceneActiveLines`, active cue density
//   `>= kMinSustainedHSceneDensity`, and erotic anchor count (`EroticVocalOrSfx`
//   or `ExplicitOrTagged`) `>= kMinSustainedHSceneAnchors`.
// - Short dirty/crude banter (`< kMinSustainedHSceneLines`) and sustained
//   combat/chase/horror panting (`< kMinSustainedHSceneAnchors` erotic anchors)
//   never form an H-scene span.
std::vector<std::pair<std::size_t, std::size_t>> detect_h_scene_spans(
    const std::vector<ScriptEntry>& entries);

// Returns a boolean mask of size `entries.size()` where `mask[i] == true` iff
// index `i` falls inside a span returned by `detect_h_scene_spans(entries)`.
std::vector<bool> compute_h_scene_line_mask(
    const std::vector<ScriptEntry>& entries);

// Returns true if `en_body` contains parenthetical/bracketed translator notes
// (`(TL: ...)`, `[Note: ...]`, `(lit. ...)`) or parenthetical romaji honorific/
// kinship glosses (`(onee-chan)`, `(oniisan)`, `(senpai)`, etc.).
bool contains_translator_romaji_note(std::string_view en_body) noexcept;

// Converts LLM-escaped `"\\r\\n"` / `"\\n"` sequences to real newlines ONLY
// when `reference_old_en` uses real newlines rather than literal `"\\n"` escapes.
std::string unescape_llm_newlines(std::string_view candidate,
                                  std::string_view reference_old_en);

// Restores engine framing onto an LLM-proposed body for `entry`:
// - Strips echoed `[Speaker]` or `Speaker:` prefixes on both `Speaker\r\nBody`
//   and inline dialogue formats.
// - Unescapes newlines consistent with `entry.current_en`.
// - Re-attaches `Speaker\r\n` prefix and leading `\n` / trailing `\@` escapes
//   present on `entry.current_en`.
std::string normalize_restored_translation(const ScriptEntry& entry,
                                           std::string_view raw_candidate);

}  // namespace spot_checker
