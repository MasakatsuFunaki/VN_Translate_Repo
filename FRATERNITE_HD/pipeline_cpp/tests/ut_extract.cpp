// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Step-1 driver: message split, classification, offset bookkeeping, script
// ordering, and the whole chain end-to-end against a synthetic bn.ypf.
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "common/util.h"
#include "extract/extractor.h"
#include "test_support.h"
#include "yuris/ypf_archive.h"

using namespace frat;
using frat::extract::classify;
using frat::extract::split_into_messages;

TEST(Extract, SplitIntoMessages_GluesTerminatorPlusClosingBracket) {
    // Alternation order: [。！？]?」 wins over the bare terminator, so ！」 is
    // ONE separator.  And the '?' is OPTIONAL, so a lone 」 separates too.
    EXPECT_EQ(split_into_messages("A。B」C！」D？E"),
              (std::vector<std::string>{"A。", "B」", "C！」", "D？", "E"}));
    EXPECT_EQ(split_into_messages("セリフ」続き。"),
              (std::vector<std::string>{"セリフ」", "続き。"}));
    EXPECT_EQ(split_into_messages("あ」い"), (std::vector<std::string>{"あ」", "い"}));
    EXPECT_EQ(split_into_messages("大智「よし、こんなもんかな」次の行。"),
              (std::vector<std::string>{"大智「よし、こんなもんかな」", "次の行。"}));
    // No terminator at all -> the whole text, unsplit.
    EXPECT_EQ(split_into_messages("ふつうの文"), (std::vector<std::string>{"ふつうの文"}));
}

TEST(Extract, SplitIntoMessages_DropsWhitespaceOnlyBuffers_IncludingU3000) {
    // The trailing U+3000 buffer is dropped by the BARE strip...
    EXPECT_EQ(split_into_messages("あ。\xE3\x80\x80"), (std::vector<std::string>{"あ。"}));
    // ...but a kept piece is pushed UNSTRIPPED, leading U+3000 and all.
    EXPECT_EQ(split_into_messages("あ。\xE3\x80\x80" "。い。"),
              (std::vector<std::string>{"あ。", "\xE3\x80\x80" "。", "い。"}));
    // The early-out returns the text verbatim when no terminator occurs.
    EXPECT_EQ(split_into_messages("\xE3\x80\x80"),
              (std::vector<std::string>{"\xE3\x80\x80"}));
}

TEST(Extract, Classify_NameVsDialogueVsNarrative) {
    EXPECT_EQ(classify("あいうえおかきく"), "name");        // 8 codepoints, no punct
    EXPECT_EQ(classify("あいうえおかきくけ"), "narrative");  // 9 codepoints
    EXPECT_EQ(classify("ABCDEFGH"), "name");                // length is CODEPOINTS...
    EXPECT_EQ(classify("あ。い"), "narrative");             // ...and rule 1 needs no punct
    EXPECT_EQ(classify("「あ」"), "dialogue");
    EXPECT_EQ(classify("（あいうえおかきくけ）"), "dialogue");
    // Short and punctuation-free wins even with a dialogue opener: rule 1 runs
    // first, so （あ） is a "name".
    EXPECT_EQ(classify("（あ）"), "name");
}

namespace {

// A YSTB whose section sizes deliberately do NOT add up, so decrypt_ystb is a
// no-op and the CP932 payload stays readable.  The 32-byte header is all zeros
// after the magic, so the scanner's first run starts at offset 33.
constexpr std::size_t kRunStart = 33;

Bytes make_script(const std::string& jp) {
    Bytes b;
    for (char c : std::string("YSTB")) b.push_back(static_cast<std::uint8_t>(c));
    b.insert(b.end(), 28, 0);
    b.push_back(0x00);
    const Bytes enc = utf8_to_cp932_replace(jp);
    b.insert(b.end(), enc.begin(), enc.end());
    b.push_back(0x00);
    return b;
}

}  // namespace

TEST(Extract, ScriptOrdering_StoryFirstThenHelpersThenSidecars) {
    // Distinct bodies so every script contributes a distinct (non-deduped) run.
    const Bytes image = frat_test::build_ypf({
        {"ysbin\\yst00001.ybn", make_script("いちばんめのぶん")},
        {"ysbin\\yst.ybn", make_script("すたぶのぶん")},
        {"ysbin\\yst00320.ybn", make_script("さんびゃくにじゅう")},
        {"ysbin\\yst_list.ybn", make_script("りすとのぶん")},
        {"ysbin\\yst00156.ybn", make_script("ひゃくごじゅうろく")},
        {"ysbin\\yst00003.ybn", make_script("さんばんめのぶん")},
    });

    frat_test::ScratchDir dir("order");
    std::filesystem::create_directories(std::filesystem::u8path(dir / "pac"));
    write_file(dir / "pac\\bn.ypf", image);

    boost::json::object data = extract::extract_from_archives(dir / "pac");
    ASSERT_TRUE(data.contains("bn.ypf"));
    const auto& strings = data.at("bn.ypf").get_object().at("strings").get_array();
    std::vector<std::string> order;
    for (const auto& s : strings) order.push_back(std::string(s.get_object().at("ybn").get_string()));
    // Story scripts (>= 156) ascending, then helpers ascending, then the
    // sidecars in YPF index order.  yst.ybn and yst_list.ybn are excluded from
    // the dialogue set but yst_list.ybn/yst.ybn are matched as sidecars.
    EXPECT_EQ(order, (std::vector<std::string>{
                         "ysbin\\yst00156.ybn", "ysbin\\yst00320.ybn", "ysbin\\yst00001.ybn",
                         "ysbin\\yst00003.ybn", "ysbin\\yst.ybn", "ysbin\\yst_list.ybn"}));
}

TEST(Extract, OffsetBookkeeping_AdvancesOverSkippedAndDuplicatePieces) {
    // One run, three pieces: a JP one, an ASCII-only one (skipped, no JP), and
    // another JP one.  piece_offset must advance across the skipped piece, so
    // the third piece's offset accounts for all preceding CP932 bytes.
    const Bytes image = frat_test::build_ypf(
        {{"ysbin\\yst00156.ybn", make_script("あ。ab。い。")}});

    frat_test::ScratchDir dir("offsets");
    std::filesystem::create_directories(std::filesystem::u8path(dir / "pac"));
    write_file(dir / "pac\\bn.ypf", image);

    boost::json::object data = extract::extract_from_archives(dir / "pac");
    const auto& strings = data.at("bn.ypf").get_object().at("strings").get_array();
    ASSERT_EQ(strings.size(), 2u);
    EXPECT_EQ(strings[0].get_object().at("text").get_string(), "あ。");
    EXPECT_EQ(strings[0].get_object().at("offset").get_int64(),
              static_cast<std::int64_t>(kRunStart));
    EXPECT_EQ(strings[0].get_object().at("byte_len").get_int64(), 4);
    // The run's trailing 。 was trimmed off by trim_ascii_edges before the
    // split (U+3002 is outside 0x3040-0x9FFF), so the tail piece is bare.
    EXPECT_EQ(strings[1].get_object().at("text").get_string(), "い");
    // run start + 4 ("あ。") + 4 ("ab。", the skipped non-JP piece).
    EXPECT_EQ(strings[1].get_object().at("offset").get_int64(),
              static_cast<std::int64_t>(kRunStart + 8));
}

TEST(Extract, GlobalSeen_DedupsAcrossFiles) {
    const Bytes blob = make_script("おなじぶんです。");
    const Bytes image = frat_test::build_ypf({{"ysbin\\yst00156.ybn", blob},
                                              {"ysbin\\yst00157.ybn", blob}});

    frat_test::ScratchDir dir("dedup");
    std::filesystem::create_directories(std::filesystem::u8path(dir / "pac"));
    write_file(dir / "pac\\bn.ypf", image);

    boost::json::object data = extract::extract_from_archives(dir / "pac");
    const auto& strings = data.at("bn.ypf").get_object().at("strings").get_array();
    EXPECT_EQ(strings.size(), 1u);
    EXPECT_EQ(strings[0].get_object().at("ybn").get_string(), "ysbin\\yst00156.ybn");
}

TEST(Extract, EndToEnd_SyntheticArchive_OutputIsPinnedByDigest) {
    // A seven-entry archive built from the checked-in ybn samples; the digest
    // pins the whole of extracted_text.json (388 entries, 88,098 bytes) --
    // CRLF, indent=2, key order, the story-first ordering, global_seen dedup
    // and the CP932 PUA decode all at once.
    const Bytes yst_plain = frat_test::sample("yst00001.ybn");
    const Bytes yst_enc = yuris::decrypt_ystb(yst_plain);  // XOR is an involution
    const Bytes cfg = frat_test::sample("yscfg.ybn");
    const Bytes yse = frat_test::sample("yse.ybn");
    const Bytes lst = frat_test::sample("yst_list.ybn");

    const Bytes image = frat_test::build_ypf({
        {"ysbin\\yst00156.ybn", yst_enc}, {"ysbin\\yst00157.ybn", yst_enc},
        {"ysbin\\yst00001.ybn", yst_enc}, {"ysbin\\yscfg.ybn", cfg},
        {"ysbin\\yse.ybn", yse},          {"ysbin\\yst_list.ybn", lst},
        {"ysbin\\yst.ybn", cfg},
    });

    frat_test::ScratchDir dir("e2e");
    std::filesystem::create_directories(std::filesystem::u8path(dir / "pac"));
    write_file(dir / "pac\\bn.ypf", image);
    const std::string out = dir / "extracted_text.json";
    ASSERT_EQ(extract::run_extract(dir / "pac", out), 0);

    const Bytes produced = read_file(out);
    EXPECT_EQ(produced.size(), 88098u);
    EXPECT_EQ(frat_test::sha256_hex(produced.data(), produced.size()),
              "2e8b27c4436e2dc2dfd3e147d69bed320b019448053b38e5d427144275eb2430");
}
