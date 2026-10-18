// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// BGI BurikoCompiledScript v1.00 text extractor (pipeline step 1).
// Classifies strings via TEXT_FCN/RUBY_FCN/BKLG_FCN (kingshriek method).
#pragma once

#include <optional>
#include <string>
#include <vector>

#include <boost/json.hpp>

#include "common/util.h"

namespace snu::extract {

struct Entry {
    std::int64_t offset = 0;   // file-absolute offset of the string
    std::int64_t byte_len = 0; // CP932 byte length
    std::string text;          // UTF-8
    bool has_jp = false;
    std::string type;          // "name" | "dialogue" | "narrative" | "other"
    std::optional<std::string> speaker;  // set on dialogue/narrative entries
};

// Walk one script's bytecode.  Returns nullopt when `data` is not a
// BurikoCompiledScriptVer1.00 file.
std::optional<std::vector<Entry>> extract_one(const Bytes& data);

// Archives in the engine's search order.  The engine takes the FIRST arc
// that holds a name, so the update patch `data01099.arc` shadows 23 scripts
// of the original arcs.  The extractor must pick the same copy, or the
// corpus holds text the engine never renders.
extern const std::vector<std::string> SCRIPT_ARCS;

// Serialise one script's entries into the extracted_text.json per-script
// object: {file, arc, file_size, jp_strings, strings:[...]}.
boost::json::object script_to_json(const std::string& name, const std::string& arc_basename,
                                   std::size_t file_size, const std::vector<Entry>& entries);

// Full step-1 run: walk GAME_DIR arcs, extract, write output_file.
// Returns process exit code.
int run_extract(const std::string& game_dir, const std::string& output_file);

}  // namespace snu::extract
