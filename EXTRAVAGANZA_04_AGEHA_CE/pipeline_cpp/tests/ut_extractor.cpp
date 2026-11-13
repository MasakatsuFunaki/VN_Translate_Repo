// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Unit tests for src/extract/extractor.{h,cpp}: directory resolution and
// end-to-end extraction of synthetic encrypted .spt files into JSON.
#include "extract/extractor.h"

#include "common/util.h"
#include "spt/spt_script.h"

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
constexpr std::size_t kAlignment = 4;
constexpr std::int64_t kExpectedStringsFound = 3;
constexpr std::int64_t kExpectedJpStrings = 2;

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

Bytes make_encrypted_spt(const std::vector<std::string>& utf8_strings) {
    Bytes dec(kHeaderSize, kSpaceByte);
    std::memcpy(dec.data() + kMagicOffset, kMagicText, kMagicLen);
    for (const auto& s : utf8_strings) {
        const Bytes enc_cp932 = utf8_to_cp932_replace(s);
        dec.insert(dec.end(), enc_cp932.begin(), enc_cp932.end());
        dec.push_back(0);
        while (dec.size() % kAlignment != 0) {
            dec.push_back(0);
        }
    }
    return spt::decrypt(dec);
}

}  // namespace

TEST(Extractor, resolve_spt_dir_prefers_backup_when_present) {
    const ScopedTempDir tmp("agh_ut_spt_resolve");
    const fs::path& base = tmp.path();
    fs::create_directories(base / "spt");
    EXPECT_EQ(extract::resolve_spt_dir(base.u8string()), (base / "spt").u8string());

    fs::create_directories(base / "spt_backup");
    EXPECT_EQ(extract::resolve_spt_dir(base.u8string()), (base / "spt_backup").u8string());
}

TEST(Extractor, run_extract_writes_classified_json_and_skips_invalid_spt) {
    const ScopedTempDir tmp("agh_ut_extract_run");
    const fs::path& base = tmp.path();
    const fs::path spt_dir = base / "spt";
    fs::create_directories(spt_dir);

    const Bytes valid_spt = make_encrypted_spt({
        "アゲハ\r\n「ごきげんよう」",
        "北畠史郎は静かに微笑んだ。",
        "bgm_01"
    });
    write_file((spt_dir / "main.spt").u8string(), valid_spt);
    write_file((spt_dir / "invalid.spt").u8string(), std::string("not an spt"));

    const std::string out_json = (base / "extracted_text.json").u8string();
    ASSERT_EQ(extract::run_extract(spt_dir.u8string(), out_json), 0);

    const bj::value root = json_parse_file(out_json);
    ASSERT_TRUE(root.is_object());
    const auto& obj = root.get_object();
    EXPECT_TRUE(obj.if_contains("main.spt"));
    EXPECT_FALSE(obj.if_contains("invalid.spt"));

    const auto& main_obj = obj.at("main.spt").get_object();
    EXPECT_EQ(main_obj.at("strings_found").as_int64(), kExpectedStringsFound);
    EXPECT_EQ(main_obj.at("jp_strings").as_int64(), kExpectedJpStrings);

    const auto& strings = main_obj.at("strings").as_array();
    ASSERT_EQ(strings.size(), static_cast<std::size_t>(kExpectedStringsFound));
    EXPECT_EQ(strings[0].as_object().at("type").as_string(), "dialogue");
    EXPECT_EQ(strings[1].as_object().at("type").as_string(), "narrative");
    EXPECT_EQ(strings[2].as_object().at("type").as_string(), "label");
}
