// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Pure unit tests for SPT XOR-0xFF decryption, header validation, 4-byte-aligned
// string extraction, bytecode noise filtering, and script line classification.
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
constexpr std::size_t kMinHeaderSize = 0x44;
constexpr std::uint8_t kSpaceByte = 0x20;
constexpr std::size_t kMagicOffset = 0x30;
constexpr const char* kMagicText = "SPTHEADER0";
constexpr std::size_t kMagicLen = 10;
constexpr std::size_t kCountOffset = 0x18;
constexpr unsigned int kShift8 = 8;
constexpr unsigned int kShift16 = 16;
constexpr unsigned int kShift24 = 24;
constexpr std::size_t kAlignment = 4;
constexpr std::uint32_t kMultiByteHeaderCount = 0x01020304u;

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
    dec[kCountOffset + 0] = static_cast<std::uint8_t>(hdr_count);
    dec[kCountOffset + 1] = static_cast<std::uint8_t>(hdr_count >> kShift8);
    dec[kCountOffset + 2] = static_cast<std::uint8_t>(hdr_count >> kShift16);
    dec[kCountOffset + 3] = static_cast<std::uint8_t>(hdr_count >> kShift24);
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

TEST(SptUnit, DecryptHeaderValidationNoiseFilterAndAlignmentBoundaries) {
    // Pinned XOR-0xFF vector across sign-bit byte seam (prevents symmetric-bug pass).
    const Bytes sign_seam = {0x00u, 0x7Fu, 0x80u, 0xFFu};
    const Bytes expected_xor = {0xFFu, 0x80u, 0x7Fu, 0x00u};
    EXPECT_EQ(spt::decrypt(sign_seam), expected_xor);
    EXPECT_EQ(spt::decrypt(expected_xor), sign_seam);

    // Header validation boundaries: truncated (< 0x44), bad magic byte, valid LE count.
    Bytes truncated(kMinHeaderSize - 1, kSpaceByte);
    EXPECT_FALSE(spt::is_valid(truncated));
    EXPECT_EQ(spt::header_string_count(Bytes(kCountOffset + 3, 0x11u)), 0u);

    Bytes bad_magic = make_spt({}, kMultiByteHeaderCount);
    bad_magic[kMagicOffset + kMagicLen - 1] = 'X';
    EXPECT_FALSE(spt::is_valid(bad_magic));
    const spt::SptScript invalid_script(spt::decrypt(bad_magic));
    EXPECT_FALSE(invalid_script.is_valid());
    EXPECT_EQ(invalid_script.header_string_count(), 0u);
    EXPECT_TRUE(invalid_script.strings().empty());

    // Valid SPT with bytecode noise ("ff", "UU"), short 3-byte ASCII ("abc" skipped),
    // invalid CP932 sequence (0x81 0x01 skipped), and real 4-byte-aligned strings.
    const Bytes invalid_cp932 = {0x81u, 0x01u, 0x81u, 0x01u};
    const Bytes dec = make_spt(
        {utf8_to_cp932_replace("ff"),
         utf8_to_cp932_replace("abc"),
         invalid_cp932,
         utf8_to_cp932_replace("夢美\r\n「いらっしゃいませ」"),
         utf8_to_cp932_replace("夢美を呼ぶ"),
         utf8_to_cp932_replace("abcd")},
        kMultiByteHeaderCount);
    const Bytes raw = spt::decrypt(dec);
    ASSERT_NE(raw, dec);

    const spt::SptScript script(raw);
    ASSERT_TRUE(script.is_valid());
    EXPECT_EQ(script.header_string_count(), kMultiByteHeaderCount);
    ASSERT_EQ(script.strings().size(), 3u);

    EXPECT_EQ(script.strings()[0].offset % kAlignment, 0u);
    EXPECT_EQ(script.strings()[0].text, "夢美\r\n「いらっしゃいませ」");
    EXPECT_EQ(script.strings()[0].type, "dialogue");
    EXPECT_TRUE(script.strings()[0].has_jp);

    EXPECT_EQ(script.strings()[1].offset % kAlignment, 0u);
    EXPECT_EQ(script.strings()[1].text, "夢美を呼ぶ");
    EXPECT_EQ(script.strings()[1].type, "narrative");
    EXPECT_TRUE(script.strings()[1].has_jp);

    EXPECT_EQ(script.strings()[2].offset % kAlignment, 0u);
    EXPECT_EQ(script.strings()[2].text, "abcd");
    EXPECT_EQ(script.strings()[2].type, "label");
    EXPECT_FALSE(script.strings()[2].has_jp);
}

TEST(SptUnit, RunExtractWritesExactClassifiedEntriesAndSkipsInvalidSpt) {
    const ScopedTempDir tmp("anzu_ce_ut_spt_extract");
    const fs::path spt_dir = tmp.path() / "spt";
    fs::create_directories(spt_dir);

    const Bytes valid_spt = spt::decrypt(make_spt({
        utf8_to_cp932_replace("夢美\r\n「いらっしゃいませ」"),
        utf8_to_cp932_replace("アゲハを呼ぶ"),
        utf8_to_cp932_replace("だが――――\r\n"),
        utf8_to_cp932_replace("bgm_01"),
    }));
    write_file((spt_dir / "tokuten.spt").u8string(), valid_spt);
    write_file((spt_dir / "invalid.spt").u8string(), std::string("not an spt"));

    const std::string out_json = (tmp.path() / "extracted_text.json").u8string();
    ASSERT_EQ(extract::run_extract(spt_dir.u8string(), out_json), 0);

    const bj::value root_v = json_parse_file(out_json);
    ASSERT_TRUE(root_v.is_object());
    const bj::object& root = root_v.get_object();
    ASSERT_EQ(root.size(), 1u);
    ASSERT_TRUE(root.if_contains("tokuten.spt") != nullptr);
    EXPECT_TRUE(root.if_contains("invalid.spt") == nullptr);

    const bj::object& tokuten_obj = root.at("tokuten.spt").as_object();
    EXPECT_EQ(tokuten_obj.at("strings_found").as_int64(), 4);
    EXPECT_EQ(tokuten_obj.at("jp_strings").as_int64(), 3);
    const bj::array& strings = tokuten_obj.at("strings").as_array();
    ASSERT_EQ(strings.size(), 4u);
    EXPECT_EQ(strings[0].as_object().at("type").as_string(), "dialogue");
    EXPECT_EQ(strings[1].as_object().at("type").as_string(), "narrative");
    EXPECT_EQ(strings[2].as_object().at("type").as_string(), "narrative");
    EXPECT_EQ(strings[3].as_object().at("type").as_string(), "label");
}

TEST(SptUnit, ClassifyDistinguishesShortChoicesAndNarrationFromNameTags) {
    struct ClassifyCase {
        std::string raw;
        const char* expected;
        const char* label;
    };
    const std::vector<ClassifyCase> cases = {
        {"", "empty", "empty-string"},
        {"   \r\n\t", "empty", "whitespace-only"},
        {"bgm_01", "label", "ascii-label"},
        {"★セーブ", "menu", "menu-prefix"},
        {"夢美\r\n「いらっしゃいませ」", "dialogue", "speaker-plate-kagikakko"},
        {"夢美\r\n『心の声』", "dialogue", "speaker-plate-double-kagikakko"},
        {"「あ」\r\n", "dialogue", "short-quoted-line"},
        {"喫茶店の扉が開き、\r\n風が吹き込んだ。\r\n", "narrative", "multiline-narration"},
        {"だが――――\r\n", "narrative", "short-narration-trailing-crlf"},
        {"夢美を呼ぶ", "narrative", "short-choice-with-hiragana"},
        {"官能。", "narrative", "short-non-hiragana-with-sentence-punct"},
        {"システム画面", "name", "exact-max-name-tag-6-chars"},
        {"システム画面表", "narrative", "above-max-name-tag-7-chars"},
        {"夢美", "name", "bare-speaker-kanji"},
        {"あ", "name", "single-char-hiragana-bytecode"},
        {std::string("｡\x01"), "other", "bytecode-with-control-char"},
        {std::string("あ\x02\r\n"), "other", "bytecode-with-control-char-and-crlf"},
    };
    for (const auto& tc : cases) {
        SCOPED_TRACE(tc.label);
        EXPECT_EQ(spt::classify(tc.raw), tc.expected);
    }
}
