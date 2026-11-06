// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Functional-contract and component tests for SPT decryption, 4-byte-aligned
// string extraction, and end-to-end extraction on WelcomeToAnzU!_CE\spt.
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include <boost/json.hpp>

#include "apps/paths.h"
#include "common/util.h"
#include "extract/extractor.h"
#include "spt/spt_script.h"

namespace fs = std::filesystem;
namespace bj = boost::json;
using namespace exm;

namespace {

constexpr std::size_t kHeaderSize = 0x80;
constexpr std::uint8_t kSpaceByte = 0x20;
constexpr std::size_t kMagicOffset = 0x30;
constexpr const char* kMagicText = "SPTHEADER0";
constexpr std::size_t kMagicLen = 10;
constexpr std::size_t kCountOffset = 0x18;
constexpr unsigned int kShift8 = 8;
constexpr unsigned int kShift16 = 16;
constexpr unsigned int kShift24 = 24;
constexpr std::size_t kAlignment = 4;
constexpr std::uint32_t kTestHeaderCount = 2u;
constexpr std::size_t kExpectedAnzUSptFiles = 3;
constexpr std::int64_t kExpectedTokutenStrings = 2558;
constexpr std::int64_t kExpectedDistinctSpeakers = 19;

Bytes make_spt(const std::vector<Bytes>& payloads, std::uint32_t hdr_count = 0) {
    Bytes dec(kHeaderSize, kSpaceByte);
    std::memcpy(dec.data() + kMagicOffset, kMagicText, kMagicLen);
    dec[kCountOffset] = static_cast<std::uint8_t>(hdr_count);
    dec[kCountOffset + 1] = static_cast<std::uint8_t>(hdr_count >> kShift8);
    dec[kCountOffset + 2] = static_cast<std::uint8_t>(hdr_count >> kShift16);
    dec[kCountOffset + 3] = static_cast<std::uint8_t>(hdr_count >> kShift24);
    for (const auto& p : payloads) {
        dec.insert(dec.end(), p.begin(), p.end());
        dec.push_back(0);
        while (dec.size() % kAlignment) dec.push_back(0);
    }
    return dec;
}

}  // namespace

TEST(SptContract, DecryptXorFfRoundTripAndPimplExtraction) {
    const Bytes dec = make_spt(
        {utf8_to_cp932_replace("夢美\r\n「いらっしゃいませ」"),
         utf8_to_cp932_replace("bgm_01")},
        kTestHeaderCount);
    const Bytes raw = spt::decrypt(dec);
    EXPECT_EQ(spt::decrypt(raw), dec);

    spt::SptScript script(raw);
    ASSERT_TRUE(script.is_valid());
    EXPECT_EQ(script.header_string_count(), kTestHeaderCount);
    ASSERT_EQ(script.strings().size(), kTestHeaderCount);
    EXPECT_EQ(script.strings()[0].offset % kAlignment, 0u);
    EXPECT_EQ(script.strings()[0].type, "dialogue");
    EXPECT_TRUE(script.strings()[0].has_jp);
    EXPECT_EQ(script.strings()[1].type, "label");
    EXPECT_FALSE(script.strings()[1].has_jp);
}

TEST(SptComponent, LiveGameExtractionProducesCompleteCorpusAndSpeakers) {
    const char* env_dir = std::getenv("VN_GAME_DIR");
    if (!env_dir || !*env_dir) {
        GTEST_SKIP() << "VN_GAME_DIR not set; skipping live SPT extraction check.";
    }
    const fs::path game_dir = fs::u8path(env_dir);
    const fs::path spt_dir = game_dir / "spt";
    if (!fs::is_directory(spt_dir)) {
        GTEST_SKIP() << "Live WelcomeToAnzU!_CE directory not present: " << spt_dir.u8string();
    }

    const fs::path tmp_out =
        fs::temp_directory_path() / "anzu_ce_ut_extracted_text.json";
    std::error_code ec;
    fs::remove(tmp_out, ec);

    ASSERT_EQ(extract::run_extract(spt_dir.u8string(), tmp_out.u8string()), 0);
    ASSERT_TRUE(fs::exists(tmp_out));

    const bj::value root_v = json_parse_file(tmp_out.u8string());
    ASSERT_TRUE(root_v.is_object());
    const bj::object& root = root_v.get_object();
    EXPECT_EQ(root.size(), kExpectedAnzUSptFiles);
    ASSERT_TRUE(root.contains("tokuten.spt"));
    EXPECT_EQ(root.at("tokuten.spt").as_object().at("strings_found").as_int64(),
              kExpectedTokutenStrings);

    std::set<std::string> speakers;
    for (const auto& kv : root) {
        for (const auto& sv : kv.value().as_object().at("strings").as_array()) {
            const auto& so = sv.as_object();
            if (const auto* sp = so.if_contains("speaker")) {
                if (sp->is_string() && !sp->as_string().empty()) {
                    speakers.emplace(sp->as_string());
                }
            }
        }
    }
    EXPECT_EQ(static_cast<std::int64_t>(speakers.size()), kExpectedDistinctSpeakers);
    EXPECT_TRUE(speakers.count("夢美") != 0);
    EXPECT_TRUE(speakers.count("アゲハ") != 0);

    fs::remove(tmp_out, ec);
}
