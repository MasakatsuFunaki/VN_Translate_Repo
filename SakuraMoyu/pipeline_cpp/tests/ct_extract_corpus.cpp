// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Component test validating the translated_text.json corpus artifact in the
// repository output directory.
#include "common/util.h"

#include <filesystem>
#include <string>

#include <boost/json.hpp>
#include <gtest/gtest.h>

namespace fs = std::filesystem;
namespace bj = boost::json;
using namespace fav;

namespace {

fs::path corpus_path() {
    return fs::u8path(FAV_PROJECT_DIR) / "script_output" / "translated_text.json";
}

}  // namespace

TEST(ExtractCorpus, deployed_extracted_json_has_entries) {
    const fs::path p = corpus_path();
    ASSERT_TRUE(fs::exists(p)) << p.u8string();
    const bj::value root = json_parse_file(p.u8string());
    ASSERT_TRUE(root.is_object());
    bool has_entries = false;
    for (const auto& kv : root.get_object()) {
        if (!kv.value().is_object()) {
            continue;
        }
        const auto& v = kv.value().get_object();
        const auto* lines = v.if_contains("lines");
        if (lines != nullptr && lines->is_array() && !lines->get_array().empty()) {
            has_entries = true;
        }
    }
    EXPECT_TRUE(has_entries) << "no archives with a non-empty lines container";
}
