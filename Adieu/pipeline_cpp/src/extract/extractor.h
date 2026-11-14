// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Japanese script extraction and contextual speaker attribution for Adieu's Scene.pck.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <boost/json.hpp>

#include "common/util.h"

namespace adi::extract {

inline constexpr std::size_t kMaxSpeakerPlateCodepoints = 12;

// True when `text` contains at least one hiragana, katakana, CJK unified, or
// CJK Extension-A codepoint. Fullwidth Latin/punctuation strings such as
// "＊Ａ" and "＊Ｂ" return false.
bool has_japanese(const std::string& text);

// True when `text` is a valid standalone speaker-plate candidate:
//   - 1..kMaxSpeakerPlateCodepoints Unicode codepoints,
//   - contains Japanese characters (`has_japanese(text)`),
//   - contains no CR/LF, quote/parenthesis wrappers, or sentence punctuation,
//   - has no leading or trailing Unicode whitespace (internal U+3000 as in
//     "睦　月" is preserved).
bool is_speaker_plate_candidate(const std::string& text);

// Context-free single-string classification:
// 'empty' | 'name' | 'dialogue' | 'narrative' | 'other'.
std::string classify_text(const std::string& text);

// Contextual per-scene string classification and speaker attribution over a
// raw scene string pool `raw_strings` (preserving raw pool indices across empty
// slots):
//   - Quoted lines (`「...」` or `『...』`) at `idx` become `"dialogue"` ONLY
//     when `raw_strings[idx - 1]` is `"＊Ａ"` (`speaker = "人見"`), `"＊Ｂ"`
//     (`speaker = "広介"`), or `is_speaker_plate_candidate` (marking `idx - 1`
//     as `"name"` and setting `speaker = raw_strings[idx - 1]`).
//   - Unattributed quoted lines become `"narrative"` with `speaker = ""`.
//   - Standalone short Japanese strings that do not precede a spoken quote
//     become `"narrative"` (except default #NAMAE seeds `"人見"` / `"広介"` in
//     `"_start"` and `"_menu"`, which stay `"name"`).
boost::json::array extract_scene_strings(const std::vector<std::string>& raw_strings,
                                         std::string_view scene_name = {});

// Proxy-DLL runtime capture log: one escaped JP string per line, deduplicated
// in first-seen order.
std::vector<std::string> ingest_capture_log(const std::string& path);

// SiglusExtract's '<idx>.<scene>.ss.txt' dump. Keyed by scene name in
// filename-sorted order; insertion order becomes the key order of
// extracted_text.json.
boost::json::object ingest_siglus_extract_dump(const std::string& dir_path);

// Full Scene.pck / fallback extraction pass. Returns a process exit code.
int run_extract(const std::string& game_dir, const std::string& output_file);

}  // namespace adi::extract
