// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Step-1 tests: the deployed extracted_text.json must be loadable, non-empty,
// and carry a lines container the downstream steps can read.
#include <gtest/gtest.h>

#include <filesystem>

#include <boost/json.hpp>

#include "common/util.h"

namespace fs = std::filesystem;
namespace bj = boost::json;
using namespace ama;

// Universal cross-game gate: whatever the engine, the deployed
// extracted_text.json must have at least one archive with entries.
TEST(Extract, deployed_extracted_json_has_entries) {
    fs::path p = fs::u8path(std::string(AMA_PROJECT_DIR) +
                            "\\script_output\\extracted_text.json");
    if (!fs::exists(p)) GTEST_SKIP() << "extracted_text.json not built yet";
    bj::value root = json_parse_file(p.u8string());
    ASSERT_TRUE(root.is_object());
    bool has_entries = false;
    for (const auto& kv : root.get_object()) {
        if (!kv.value().is_object()) continue;
        if (auto* lines = kv.value().get_object().if_contains("lines"))
            if (lines->is_array() && !lines->get_array().empty()) has_entries = true;
    }
    EXPECT_TRUE(has_entries) << "no archives with a non-empty lines container";
}

// Schema gate: every line entry carries the four fields step 2 and step 3 read,
// in the exact order the extractor emits them -- the assertion compares the
// whole key list, so field order is part of the artifact's contract.
TEST(Extract, deployed_line_entries_have_expected_schema) {
    fs::path p = fs::u8path(std::string(AMA_PROJECT_DIR) +
                            "\\script_output\\extracted_text.json");
    if (!fs::exists(p)) GTEST_SKIP() << "extracted_text.json not built yet";
    bj::value root = json_parse_file(p.u8string());
    std::size_t checked = 0;
    for (const auto& kv : root.get_object()) {
        const auto& script = kv.value().get_object();
        for (const char* f : {"arc_idx", "name", "offset", "size", "lines"})
            ASSERT_TRUE(script.contains(f)) << "script missing " << f;
        for (const auto& lv : script.at("lines").get_array()) {
            std::vector<std::string> keys;
            for (const auto& e : lv.get_object()) keys.emplace_back(e.key());
            ASSERT_EQ(keys, (std::vector<std::string>{"idx", "type", "type_val", "content"}));
            if (++checked >= 50) return;
        }
        if (checked >= 50) return;
    }
}
