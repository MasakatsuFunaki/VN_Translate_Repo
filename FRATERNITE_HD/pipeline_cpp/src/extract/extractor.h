// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Step-1 driver: message split, classification, global de-dup, JSON assembly
// and the summary log.
#pragma once

#include <cstddef>
#include <set>
#include <string>
#include <vector>

#include <boost/json.hpp>

namespace frat::extract {

// yst00000-yst00155 are engine/UI helper scripts; yst00156+ is the narrative.
// Story scripts are emitted first so the JSON follows narrative order.
constexpr int STORY_SCRIPT_START = 156;

// YBN sidecars that store plaintext strings (membership test only).
const std::set<std::string>& plaintext_ybn_basenames();

struct Run {
    std::string ybn;      // entry name with its ORIGINAL case
    std::size_t offset;   // byte offset of the run inside the decrypted YBN
    std::string text;
};

std::vector<Run> extract_ystb_dialogue(const std::string& ypf_path);
std::vector<Run> extract_plaintext_sidecars(const std::string& ypf_path);

// YU-RIS packs a paragraph back-to-back with no separator byte; the engine
// renders one on-screen message per JP terminator, so the proxy DLL can only
// match if we emit one entry per message.
//
// Separator = ([。！？]?」|[。！？]), i.e. THREE shapes, tried in this order at
// each position:
//   1. one of 。！？ followed by 」   (2 codepoints)
//   2. a bare 」                      (1 codepoint -- the '?' is OPTIONAL)
//   3. one of 。！？                  (1 codepoint)
// Missing case 2 silently changes every synthetic offset downstream.
std::vector<std::string> split_into_messages(const std::string& text);

// Hiragana / katakana / CJK unified + ext-A only -- no fullwidth forms.
bool has_japanese(const std::string& t);

// "name" / "dialogue" / "narrative", evaluated in that order.
std::string classify(const std::string& t);

boost::json::object extract_from_archives(const std::string& pac_dir);

int run_extract(const std::string& pac_dir, const std::string& output_file);

}  // namespace frat::extract
