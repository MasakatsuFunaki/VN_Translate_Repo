// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Pure standalone unit tests for SPT decryption, 4-byte-aligned string
// extraction, and synthetic .spt directory extraction into JSON.
#include "spt/spt_script.h"

#include "common/util.h"
#include "extract/extractor.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

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
constexpr std::size_t kByteIndex0 = 0;
constexpr std::size_t kByteIndex1 = 1;
constexpr std::size_t kByteIndex2 = 2;
constexpr std::size_t kByteIndex3 = 3;
constexpr std::size_t kAlignment = 4;
constexpr std::uint32_t kTestHeaderCount = 2u;
constexpr std::int64_t kExpectedSyntheticStrings = 3;
constexpr std::int64_t kExpectedSyntheticJpStrings = 2;

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view name)
        : path_(fs::temp_directory_path() / fs::u8path(name)) {
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

Bytes make_spt(const std::vector<Bytes>& payloads, std::uint32_t hdr_count = 0) {
    Bytes dec(kHeaderSize, kSpaceByte);
    std::memcpy(dec.data() + kMagicOffset, kMagicText, kMagicLen);
    dec[kCountOffset + kByteIndex0] = static_cast<std::uint8_t>(hdr_count);
    dec[kCountOffset + kByteIndex1] = static_cast<std::uint8_t>(hdr_count >> kShift8);
    dec[kCountOffset + kByteIndex2] = static_cast<std::uint8_t>(hdr_count >> kShift16);
    dec[kCountOffset + kByteIndex3] = static_cast<std::uint8_t>(hdr_count >> kShift24);
    for (const auto& p : payloads) {
        dec.insert(dec.end(), p.begin(), p.end());
        dec.push_back(0);
        while (dec.size() % kAlignment != 0) {
            dec.push_back(0);
        }
    }
    return dec;
}

}  // namespace

TEST(SptUnit, DecryptXorFfRoundTripAndPimplExtraction) {
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

TEST(SptUnit, RunExtractWritesClassifiedJsonFromSyntheticSptDirectory) {
    const ScopedTempDir tmp("anzu_ce_ut_spt_extract");
    const fs::path spt_dir = tmp.path() / "spt";
    fs::create_directories(spt_dir);

    const Bytes valid_spt = spt::decrypt(make_spt({
        utf8_to_cp932_replace("夢美\r\n「いらっしゃいませ」"),
        utf8_to_cp932_replace("喫茶ＡｎｚＵの扉が開いた。"),
        utf8_to_cp932_replace("bgm_01"),
    }));
    write_file((spt_dir / "tokuten.spt").u8string(), valid_spt);
    write_file((spt_dir / "invalid.spt").u8string(), std::string("not an spt"));

    const std::string out_json = (tmp.path() / "extracted_text.json").u8string();
    ASSERT_EQ(extract::run_extract(spt_dir.u8string(), out_json), 0);

    const bj::value root_v = json_parse_file(out_json);
    ASSERT_TRUE(root_v.is_object());
    const bj::object& root = root_v.get_object();
    ASSERT_TRUE(root.if_contains("tokuten.spt") != nullptr);
    EXPECT_TRUE(root.if_contains("invalid.spt") == nullptr);

    const bj::object& tokuten_obj = root.at("tokuten.spt").as_object();
    EXPECT_EQ(tokuten_obj.at("strings_found").as_int64(), kExpectedSyntheticStrings);
    EXPECT_EQ(tokuten_obj.at("jp_strings").as_int64(), kExpectedSyntheticJpStrings);
}
