// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Step 1: extract JP text from YPF archives into extracted_text.json.
#pragma once

#include <cstddef>
#include <set>
#include <string>
#include <vector>

#include <boost/json.hpp>

namespace frat::extract {

// yst00156+ is narrative; story scripts are emitted first.
constexpr int STORY_SCRIPT_START = 156;

const std::set<std::string>& plaintext_ybn_basenames();

struct Run {
    std::string ybn;      // entry name with its ORIGINAL case
    std::size_t offset;   // byte offset of the run inside the decrypted YBN
    std::string text;
};

std::vector<Run> extract_ystb_dialogue(const std::string& ypf_path);
std::vector<Run> extract_plaintext_sidecars(const std::string& ypf_path);

// Split at JP terminators ([。！？]?」|[。！？]) — one entry per on-screen message.
std::vector<std::string> split_into_messages(const std::string& text);

bool has_japanese(const std::string& t);
std::string classify(const std::string& t);

boost::json::object extract_from_archives(const std::string& pac_dir);

int run_extract(const std::string& pac_dir, const std::string& output_file);

}  // namespace frat::extract
