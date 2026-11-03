// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Step-1 tests: extracted_text.json must be loadable, non-empty, and carry a
// lines container the downstream steps can read.
#include <gtest/gtest.h>

#include <filesystem>

#include <boost/json.hpp>

#include "common/util.h"
#include "extract/extractor.h"
#include "fvp/fvp_hcb.h"

namespace fs = std::filesystem;
namespace bj = boost::json;
using namespace fav;

namespace {

Bytes make_hcb_with_strings(const std::vector<Bytes>& payloads) {
    Bytes body;
    for (const auto& p : payloads) {
        body.push_back(0x0E);
        body.push_back(static_cast<std::uint8_t>(p.size() + 1));
        body.insert(body.end(), p.begin(), p.end());
        body.push_back(0x00);
    }
    Bytes out;
    const std::uint32_t len = static_cast<std::uint32_t>(4 + body.size());
    out.push_back(static_cast<std::uint8_t>(len & 0xFF));
    out.push_back(static_cast<std::uint8_t>((len >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((len >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((len >> 24) & 0xFF));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

}  // namespace

TEST(Extract, writes_schema_valid_json) {
    const std::string hcb = (fs::temp_directory_path() / "fav_ut.hcb").u8string();
    const std::string out = (fs::temp_directory_path() / "fav_ut_extracted.json").u8string();
    // 'あ' (JP) then 'ok' (ASCII) -- both kept; the translator step is the
    // one that decides what's worth sending to the API.
    write_file(hcb, make_hcb_with_strings({Bytes{0x82, 0xA0}, Bytes{'o', 'k'}}));

    ASSERT_EQ(extract::run_extract(hcb, out), 0);

    bj::value root = json_parse_file(out);
    ASSERT_TRUE(root.is_object());
    const auto& o = root.get_object();
    ASSERT_TRUE(o.contains("Sakura.hcb"));
    const auto& script = o.at("Sakura.hcb").get_object();
    EXPECT_EQ(script.at("name").get_string(), "Sakura.hcb");
    const auto& lines = script.at("lines").get_array();
    ASSERT_EQ(lines.size(), 2u);

    const auto& l0 = lines[0].get_object();
    EXPECT_EQ(l0.at("idx").get_int64(), 0);
    EXPECT_EQ(l0.at("type").get_string(), "STRING");
    EXPECT_EQ(l0.at("offset").get_int64(), 4);
    EXPECT_EQ(l0.at("content").get_string(), "\xE3\x81\x82");
    EXPECT_EQ(lines[1].get_object().at("content").get_string(), "ok");

    // Key order is part of the artefact's contract: the DLL-side tooling and
    // the run-to-run diffs of extracted_text.json both read it positionally.
    std::vector<std::string> keys;
    for (const auto& kv : l0) keys.emplace_back(kv.key());
    EXPECT_EQ(keys, (std::vector<std::string>{"idx", "type", "offset", "content"}));

    fs::remove(fs::u8path(hcb));
    fs::remove(fs::u8path(out));
}

// Universal cross-game gate: whatever the engine, the deployed
// extracted_text.json must have at least one archive with entries.
TEST(Extract, deployed_extracted_json_has_entries) {
    fs::path p = fs::u8path(std::string(FAV_PROJECT_DIR) +
                            "\\script_output\\extracted_text.json");
    if (!fs::exists(p)) GTEST_SKIP() << "extracted_text.json not built yet";
    bj::value root = json_parse_file(p.u8string());
    ASSERT_TRUE(root.is_object());
    bool has_entries = false;
    for (const auto& kv : root.get_object()) {
        if (!kv.value().is_object()) continue;
        const auto& v = kv.value().get_object();
        if (auto* lines = v.if_contains("lines"))
            if (lines->is_array() && !lines->get_array().empty()) has_entries = true;
    }
    EXPECT_TRUE(has_entries) << "no archives with a non-empty lines container";
}
