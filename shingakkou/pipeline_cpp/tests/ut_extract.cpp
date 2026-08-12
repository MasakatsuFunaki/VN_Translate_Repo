// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Step-1 tests: the marker walk, the pre-byte classification order and the
// speaker split whose length bound is the single most expensive thing in this
// port to get wrong.
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include <openssl/evp.h>

#include "common/util.h"
#include "extract/extractor.h"
#include "translate/glossary.h"
#include "translate/translate_core.h"

namespace fs = std::filesystem;
using namespace shin;
namespace ex = shin::extract;

namespace {

// UTF-8 source literal -> the UTF-16LE bytes the engine actually stores.
void append_utf16le(Bytes& out, const std::string& utf8) {
    std::size_t i = 0;
    while (i < utf8.size()) {
        const char32_t cp = utf8_next(utf8, i);
        ASSERT_LT(cp, 0x10000u) << "test fixtures are BMP-only";
        out.push_back(static_cast<std::uint8_t>(cp & 0xFF));
        out.push_back(static_cast<std::uint8_t>(cp >> 8));
    }
}

// <pre_byte> FF 01 80 <UTF-16LE> 00 00
void append_record(Bytes& out, std::uint8_t pre_byte, const std::string& utf8) {
    out.push_back(pre_byte);
    out.push_back(0xFF);
    out.push_back(0x01);
    out.push_back(0x80);
    append_utf16le(out, utf8);
    out.push_back(0x00);
    out.push_back(0x00);
}

std::vector<ex::Str> one(std::uint8_t pre_byte, const std::string& utf8) {
    Bytes data;
    append_record(data, pre_byte, utf8);
    return ex::extract_strings_from_script(data);
}

std::string sha256_file(const std::string& path) {
    const Bytes raw = read_file(path);
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int md_len = 0;
    EVP_Digest(raw.data(), raw.size(), md, &md_len, EVP_sha256(), nullptr);
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (unsigned int i = 0; i < md_len; ++i) {
        out += hex[md[i] >> 4];
        out += hex[md[i] & 0xF];
    }
    return out;
}

}  // namespace

// THE 311-LINE REGRESSION.  The speaker bound is on codepoints; this name is
// 10 codepoints but 30 bytes, so a byte-length test files it as narration with
// an empty speaker and every one of its TSV keys changes shape.
TEST(Extract, speaker_split_accepts_long_japanese_names) {
    const auto s = one(0x36, "ミスター・マコーリー\\n「x」");
    ASSERT_EQ(s.size(), 1u);
    EXPECT_EQ(s[0].type, "dialogue");
    EXPECT_EQ(s[0].speaker, "ミスター・マコーリー");
    EXPECT_EQ(s[0].text, "「x」");
}

TEST(Extract, speaker_split_boundary_is_twenty_codepoints) {
    // Exactly 20 -> still a speaker.
    std::string name20;
    for (int i = 0; i < 20; ++i) name20 += "あ";
    const auto ok = one(0x36, name20 + "\\n「x」");
    ASSERT_EQ(ok.size(), 1u);
    EXPECT_EQ(ok[0].type, "dialogue");
    EXPECT_EQ(ok[0].speaker, name20);

    // 21 -> falls through to narration, and the WHOLE string is the content.
    const std::string name21 = name20 + "あ";
    const auto bad = one(0x36, name21 + "\\n「x」");
    ASSERT_EQ(bad.size(), 1u);
    EXPECT_EQ(bad[0].type, "narration");
    EXPECT_EQ(bad[0].speaker, "");
    EXPECT_EQ(bad[0].text, name21 + "\\n「x」");
}

// A leading literal backslash-n skips the speaker split entirely, then has
// exactly two characters removed.  A doubled prefix must keep the second --
// that is what makes the TSV key '\n' + text round-trip.
TEST(Extract, narration_strips_only_one_leading_backslash_n) {
    const auto s = one(0x36, "\\n\\nあいう");
    ASSERT_EQ(s.size(), 1u);
    EXPECT_EQ(s[0].type, "narration");
    EXPECT_EQ(s[0].text, "\\nあいう");
}

TEST(Extract, prebyte_classification_order) {
    // 0x01 + Japanese -> choice, and it never reaches the 0x36/0x07 gate.
    const auto choice = one(0x01, "屋上");
    ASSERT_EQ(choice.size(), 1u);
    EXPECT_EQ(choice[0].type, "choice");
    EXPECT_EQ(choice[0].speaker, "");

    EXPECT_TRUE(one(0x01, "ascii only").empty());  // 0x01 without Japanese: dropped
    EXPECT_EQ(one(0x36, "あいう").size(), 1u);
    EXPECT_EQ(one(0x07, "あいう").size(), 1u);
    for (int pre : {0x00, 0x02, 0x04})
        EXPECT_TRUE(one(static_cast<std::uint8_t>(pre), "あいう").empty());
}

// The two Japanese predicates are deliberately different functions; using
// either one in the other's place changes what gets extracted or translated.
TEST(Extract, has_japanese_is_wider_than_the_translate_predicate) {
    EXPECT_TRUE(ex::has_japanese("？？？"));                     // fullwidth
    EXPECT_FALSE(translate::has_real_japanese("？？？"));
    EXPECT_FALSE(ex::has_japanese("\xE3\x90\x80"));              // U+3400 CJK Ext-A
    EXPECT_TRUE(translate::has_real_japanese("\xE3\x90\x80"));
}

// Pins `i = pos + 2` (unconditional, before the filters) and the `pos + 1 < n`
// inner bound: an off-by-one here loses every second record or spins forever.
TEST(Extract, walk_advances_past_the_terminator) {
    Bytes data;
    append_record(data, 0x36, "\\nいち");
    const std::size_t second_marker = data.size() + 1;
    append_record(data, 0x36, "\\nに");
    const auto s = ex::extract_strings_from_script(data);
    ASSERT_EQ(s.size(), 2u);
    EXPECT_EQ(s[0].offset, 1u);
    EXPECT_EQ(s[1].offset, second_marker);
    EXPECT_EQ(s[0].text, "いち");
    EXPECT_EQ(s[1].text, "に");
}

TEST(Extract, short_buffer_does_not_underflow) {
    for (std::size_t n = 0; n < 4; ++n)
        EXPECT_TRUE(ex::extract_strings_from_script(Bytes(n, 0xFF)).empty());
}

// END-TO-END over the real archive: one assertion pinning every classification
// and encoding rule in step 1 at once.  Any change to the walker, the speaker
// split or the JSON format moves this hash, and moving it means the TSV keys
// the proxy DLL matches on have changed too.  Skipped when the game is not
// installed.
TEST(Extract, golden_extract_is_pinned_by_sha256) {
    const std::string game = "D:\\Kits\\my_downloadedGames\\+new\\Dlsite\\Shingakkou";
    if (!fs::exists(fs::u8path(ex::resolve_source_file(game))))
        GTEST_SKIP() << "game not installed";

    const std::string out =
        (fs::temp_directory_path() / "shin_ut_extracted.json").u8string();
    ASSERT_EQ(ex::run_extract(game, out), 0);
    EXPECT_EQ(fs::file_size(fs::u8path(out)), 9474419u);
    EXPECT_EQ(sha256_file(out),
              "423a35d7079104e10af1332b92f34e29281dc2d8fc184d3fd06e5dec2d933d5f");
    fs::remove(fs::u8path(out));
}

// The 91 STORY_ORDER names must be an exact set match with the archive's
// script names; a leftover or a typo would silently drop a file to the tail
// of the translation order.
TEST(Extract, story_order_matches_the_archive_scripts) {
    const std::string extracted =
        std::string(SHIN_PROJECT_DIR) + "\\script_output\\extracted_text.json";
    if (!fs::exists(fs::u8path(extracted))) GTEST_SKIP() << "no reference extraction";
    boost::json::value v = json_parse_file(extracted);
    const auto& o = v.get_object();
    EXPECT_EQ(o.size(), translate::story_order().size());
    for (const auto& f : translate::story_order())
        EXPECT_TRUE(o.if_contains(f)) << "STORY_ORDER names a missing script: " << f;
}
