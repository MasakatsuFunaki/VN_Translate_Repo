// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Step 1: extract Japanese text from Sakura no Toki Artemis-engine .ast
// scripts.
//
// Source: sakuranotoki.pfs (+ .000, .001, .002, .010 patches).  Each .ast
// file is XOR-encrypted Lua source -- see artemis/pfs_archive.h.
//
// Decrypted .ast layout (machine-generated Lua):
//
//     astver = 2.0
//     ast = {
//         block_NNNNN = {
//             {"text"},
//             text = {
//                 vo = { {"vo", file=...} },
//                 ja = {
//                     {
//                         name = {"<speaker>"},
//                         "<dialogue line>",         <-- bare quoted string
//                         "<continuation>",          <-- (optional) more
//                         {"rt2"},
//                     },
//                 },
//             },
//         },
//     }
//
// We capture Japanese-bearing strings at three syntactic positions:
//   1. `name = {"..."}`               speaker name       (type=name)
//   2. bare `"...",` on its own line inside ja={}         (dialogue/narrative)
//   3. `{"savetitle", text="..."}`    chapter title      (type=title)
//
// A name plate is a Lua list and can carry a second element:
//
//     name = {"藍", "？？？"},
//
// The first element is the speaker the script knows, the second is what the
// name box shows while the scene keeps that character unnamed.  The first
// element is the plate's text; the second becomes its `display`.
//
// The plate voices the lines after it, so every dialogue and narrative entry
// is written with the plate that was standing when the engine reached it.
// The translate step reads that field instead of rebuilding the state itself.
//
// and deliberately SKIP `file=`, `path=`, `ch=`, `head=`, `face=`, `func=`,
// `bgm`/`bg`/`fg`/`vo` engine commands -- those are asset handles, not
// user-facing text.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "common/util.h"

namespace snt::extract {

struct StringEntry {
    int offset = 0;  // 1-based line number
    std::string text;
    std::string type;  // dialogue | narrative | name | title
    // The Japanese plate that voices this entry, as the script spells it.
    // Only a `dialogue` or `narrative` entry carries one, and nullopt means
    // the line is narration.
    std::optional<std::string> speaker;
    // What the name box shows when it differs from the plate's speaker, empty
    // otherwise.  Only a `name` entry carries one.
    std::string display;
};

// Walk decrypted Lua source line by line, capturing JP strings.
// Returns nullopt when the payload is not an Artemis .ast file.
std::optional<std::vector<StringEntry>> extract_one(const Bytes& plaintext);

// Full step-1 run over every .ast in the archive overlay.
int run_extract(const std::string& game_dir, const std::string& output_file);

}  // namespace snt::extract
