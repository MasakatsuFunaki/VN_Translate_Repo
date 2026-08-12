// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Step-1 tests.  Most of these pin bugs that shipped and rendered raw
// Japanese in-game; see the CLAUDE.md sections referenced per case.
#include <gtest/gtest.h>

#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include <boost/json.hpp>

#include "common/util.h"
#include "extract/extractor.h"

namespace bj = boost::json;
namespace fs = std::filesystem;
using namespace crc;
namespace ex = crc::extract;

namespace {

// CP932 fragments used to build synthetic bytecode.
constexpr std::uint8_t CP_OPEN[] = {0x81, 0x75};             // 「
constexpr std::uint8_t CP_CLOSE[] = {0x81, 0x76};            // 」
constexpr std::uint8_t CP_KAERU[] = {0x8B, 0x41, 0x82, 0xE9};      // 帰る
constexpr std::uint8_t CP_TAICHI[] = {0x91, 0xBE, 0x88, 0xEA};    // 太一
constexpr std::uint8_t CP_KYOUWA[] = {0x8D, 0xA1, 0x93, 0xFA, 0x82, 0xCD};  // 今日は

void put(Bytes& b, const std::uint8_t* p, std::size_t n) { b.insert(b.end(), p, p + n); }
template <std::size_t N>
void put(Bytes& b, const std::uint8_t (&a)[N]) {
    put(b, a, N);
}
void put(Bytes& b, std::initializer_list<std::uint8_t> l) { b.insert(b.end(), l); }

}  // namespace

// ── CLAUDE.md §6 ──────────────────────────────────────────────────────────
TEST(Extract, StripControlPrefixKeepsPurePrintablePrefix) {
    const char* table[] = {
        "\xE2\x80\xBB\xE7\x94\xB7\xE5\xBF\x83\xE3\x82\x92\xE9\xB7\xB2\xE6\x8E\xB4\xE3\x82\x80"
        "\xE2\x80\xA6",                                                         // ※男心を鷲掴む…
        "\xEF\xBC\xA1\xE5\xAE\x9A\xE9\xA3\x9F\xE3\x81\xAF\xE4\xB8\x89\xE6\x99\x82\xE9\x96\x93"
        "\xE7\x9B\xAE",                                                          // Ａ定食は三時間目
        "\xEF\xBC\x88\xE4\xBD\x90\xE5\x80\x89\xEF\xBC\x89\xEF\xBC\x88\xE9\x81\x8A\xE7\xB4\x97"
        "\xEF\xBC\x89",                                                          // （佐倉）（遊紗）
        "FLOWER'S\xE3\x81\xAE\xE3\x82\x82\xE3\x81\x86\xE7\x89\x87\xE6\x96\xB9",  // FLOWER'Sのもう片方
        "\xE3\x80\x8C\xE3\x81\x82\xE3\x80\x8D",                                  // 「あ」
        "\xE2\x80\xA6\xE5\xA7\x8B\xE3\x81\xBE\xE3\x82\x8A",                      // …始まり
        "ABCDEF",
        "",
    };
    for (const char* s : table) EXPECT_EQ(ex::strip_control_prefix(s), s) << s;
}

TEST(Extract, StripControlPrefixStripsWhenPrefixHasControlByte) {
    EXPECT_EQ(ex::strip_control_prefix("\x05" "ABC\xE3\x80\x8C\xE3\x81\x82\xE3\x80\x8D"),
              "\xE3\x80\x8C\xE3\x81\x82\xE3\x80\x8D");
    EXPECT_EQ(ex::strip_control_prefix("\x18\x02\xE3\x81\x82\xE3\x81\x84\xE3\x81\x86"),
              "\xE3\x81\x82\xE3\x81\x84\xE3\x81\x86");
    EXPECT_EQ(ex::strip_control_prefix("\x01" "abc\xE3\x80\x8C\xE3\x81\x8A\xE3\x80\x8D"),
              "\xE3\x80\x8C\xE3\x81\x8A\xE3\x80\x8D");
}

// ── CLAUDE.md §9 ──────────────────────────────────────────────────────────
TEST(Extract, ExtractStringsSlidesOneByteOnDecodeFailure) {
    // 0x86 0x3A is an invalid CP932 pair (0x3A is ASCII ':', not a trail
    // byte), so strict decode fails at offset 0 of the chunk.  Jumping to the
    // next NUL here would lose the dialogue that follows in the same chunk.
    Bytes data;
    put(data, {0x86, 0x3A, 0x59, 0x45, 0xFF, 0xFF, 0x63, 0x01});
    put(data, CP_OPEN);
    put(data, CP_KYOUWA);
    put(data, CP_CLOSE);
    put(data, {0x00});

    const auto strings = ex::extract_strings(data, {});
    bool found = false;
    for (const auto& s : strings)
        if (s.text.find("\xE4\xBB\x8A\xE6\x97\xA5\xE3\x81\xAF") != std::string::npos) found = true;
    EXPECT_TRUE(found) << "the 「今日は」 chunk was dropped -- the walker skipped past the NUL";
}

// ── CLAUDE.md §7 ──────────────────────────────────────────────────────────
TEST(Extract, ClassifyKeepsTwoCharChoiceOptionBehindPreamble) {
    // FF FF <label LE16> 00 00 帰る 00  -- the choice-option shape.
    Bytes with_preamble;
    put(with_preamble, {0xFF, 0xFF, 0x12, 0x34, 0x00, 0x00});
    put(with_preamble, CP_KAERU);
    put(with_preamble, {0x00});
    auto entries = ex::classify_and_pair(ex::extract_strings(with_preamble, {}), with_preamble);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].type, "narration");
    EXPECT_EQ(entries[0].text, "\xE5\xB8\xB0\xE3\x82\x8B");  // 帰る

    // Same two characters with no preamble stay below the >= 3 floor.
    Bytes bare;
    put(bare, CP_KAERU);
    put(bare, {0x00});
    EXPECT_TRUE(ex::classify_and_pair(ex::extract_strings(bare, {}), bare).empty());
}

TEST(Extract, IsChoiceOptionAtBoundsCheck) {
    const Bytes data(4, 0xFF);
    for (std::size_t off = 0; off < 6; ++off) EXPECT_FALSE(ex::is_choice_option_at(off, data));
}

TEST(Extract, FindSpeakerOffsetsAdvancesByThreeAndBoundsNameLength) {
    Bytes data;
    put(data, {0x47, 0x0D, 0x00});
    put(data, CP_TAICHI);
    put(data, {0x00});
    const auto offs = ex::find_speaker_offsets(data);
    ASSERT_EQ(offs.size(), 1u);
    EXPECT_EQ(*offs.begin(), 3u);

    // A 31-byte name is over the limit and must be rejected.
    Bytes too_long;
    put(too_long, {0x47, 0x0D, 0x00});
    for (int i = 0; i < 31; ++i) too_long.push_back('a');
    too_long.push_back(0x00);
    EXPECT_TRUE(ex::find_speaker_offsets(too_long).empty());

    // A zero-length name (marker immediately followed by NUL) is rejected too:
    // the marker's own trailing 00 is the terminator.
    const Bytes empty_name{0x47, 0x0D, 0x00, 0x00};
    EXPECT_TRUE(ex::find_speaker_offsets(empty_name).empty());

    // No terminator at all -> skipped, not crashed.
    const Bytes unterminated{0x47, 0x0D, 0x00, 'a', 'b'};
    EXPECT_TRUE(ex::find_speaker_offsets(unterminated).empty());
}

TEST(Extract, SpeakerPairsWithNextEntryContainingOpenQuote) {
    // 47 0D 00 太一 00 「今日は」 00
    Bytes data;
    put(data, {0x47, 0x0D, 0x00});
    put(data, CP_TAICHI);
    put(data, {0x00});
    put(data, CP_OPEN);
    put(data, CP_KYOUWA);
    put(data, CP_CLOSE);
    put(data, {0x00});

    const auto speakers = ex::find_speaker_offsets(data);
    const auto entries = ex::classify_and_pair(ex::extract_strings(data, speakers), data);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].type, "dialogue");
    EXPECT_EQ(entries[0].speaker, "\xE5\xA4\xAA\xE4\xB8\x80");  // 太一
    EXPECT_EQ(entries[0].offset, 8u);                           // the DIALOGUE offset

    // An unpaired speaker name is dropped entirely -- no entry at all.
    Bytes lonely;
    put(lonely, {0x47, 0x0D, 0x00});
    put(lonely, CP_TAICHI);
    put(lonely, {0x00});
    const auto lonely_speakers = ex::find_speaker_offsets(lonely);
    EXPECT_TRUE(
        ex::classify_and_pair(ex::extract_strings(lonely, lonely_speakers), lonely).empty());
}

TEST(Extract, JapaneseDensityCountsCodepointsAndBonusChars) {
    // …！？ -- all three are bonus characters, none is a byte-length 1/3.
    EXPECT_DOUBLE_EQ(ex::japanese_density("\xE2\x80\xA6\xEF\xBC\x81\xEF\xBC\x9F"), 1.0);
    EXPECT_DOUBLE_EQ(ex::japanese_density(""), 0.0);
    // あいう + 3 ASCII -> 3/6.
    EXPECT_DOUBLE_EQ(ex::japanese_density("\xE3\x81\x82\xE3\x81\x84\xE3\x81\x86" "abc"), 0.5);
}

TEST(Extract, IsRealJapaneseIncludesCjkSymbolsExcludesHalfwidthKana) {
    EXPECT_TRUE(ex::is_real_japanese(0x3001));   // 、 (CJK symbols)
    EXPECT_TRUE(ex::is_real_japanese(0x300C));   // 「
    EXPECT_TRUE(ex::is_real_japanese(0x3042));   // あ
    EXPECT_TRUE(ex::is_real_japanese(0x4E00));   // 一
    EXPECT_FALSE(ex::is_real_japanese(0xFF71));  // ｱ  half-width katakana
    EXPECT_FALSE(ex::is_real_japanese(0xFF9D));  // ﾝ
    EXPECT_FALSE(ex::is_real_japanese(0xFF21));  // Ａ fullwidth latin
}

TEST(Extract, RotationPutsFirstEntryAtOrAfterGameStartOffset) {
    EXPECT_EQ(ex::GAME_START_OFFSET, 1891283u);

    std::vector<ex::Entry> entries = {
        {10, "narration", "", "a"},
        {ex::GAME_START_OFFSET, "narration", "", "b"},
        {ex::GAME_START_OFFSET + 5, "narration", "", "c"},
    };
    EXPECT_EQ(ex::rotate_to_game_start(entries), 1u);
    EXPECT_EQ(entries[0].text, "b");
    EXPECT_EQ(entries[1].text, "c");
    EXPECT_EQ(entries[2].text, "a");

    // No entry reaches the offset -> start_idx 0 -> list untouched.
    std::vector<ex::Entry> low = {{1, "narration", "", "a"}, {2, "narration", "", "b"}};
    EXPECT_EQ(ex::rotate_to_game_start(low), 0u);
    EXPECT_EQ(low[0].text, "a");
}

// Real-data invariants over the committed step-1 artefact.  Every one of these
// is a gate on the TSV keys the runtime DLL will look up: a violation means raw
// Japanese on screen, not a crash, so nothing else would catch it.
TEST(Extract, DeployedExtractedJsonInvariants) {
    const fs::path p =
        fs::u8path(std::string(CRC_PROJECT_DIR) + "\\script_output\\extracted_text.json");
    if (!fs::exists(p)) GTEST_SKIP() << "extracted_text.json not present";

    const bj::value root = json_parse_file(p.u8string());
    ASSERT_TRUE(root.is_object());
    ASSERT_TRUE(root.get_object().if_contains("sn.bin"));
    const auto& block = root.get_object().at("sn.bin").get_object();
    ASSERT_TRUE(block.if_contains("strings"));
    const auto& strings = block.at("strings").get_array();
    ASSERT_FALSE(strings.empty());
    for (const char* k : {"offset", "type", "speaker", "text"})
        EXPECT_TRUE(strings[0].get_object().if_contains(k)) << k;

    std::set<std::int64_t> offsets;
    std::size_t ctrl = 0, dup = 0, not_cp932 = 0, bad_speaker = 0;
    for (const auto& sv : strings) {
        const auto& s = sv.get_object();
        const std::string text(s.at("text").get_string());
        const std::string speaker(s.at("speaker").get_string());

        if (!offsets.insert(s.at("offset").get_int64()).second) ++dup;

        std::size_t i = 0;
        while (i < text.size()) {
            const char32_t cp = utf8_next(text, i);
            if (cp < 0x20 && cp != '\r' && cp != '\n' && cp != '\t') {
                ++ctrl;
                break;
            }
        }
        // The DLL converts each key UTF-8 -> CP932 before matching the bytes
        // the engine emits; a key that will not encode is a guaranteed miss.
        if (!utf8_to_cp932_strict(text)) ++not_cp932;

        std::size_t sp_len = 0, j = 0;
        while (j < speaker.size()) {
            utf8_next(speaker, j);
            ++sp_len;
        }
        if (!speaker.empty() &&
            (speaker.find("\xE3\x80\x8C") != std::string::npos || sp_len > 20))
            ++bad_speaker;
    }
    EXPECT_EQ(ctrl, 0u) << "entries with control bytes in text";
    EXPECT_EQ(dup, 0u) << "duplicate offsets";
    EXPECT_EQ(not_cp932, 0u) << "keys that do not encode to CP932";
    EXPECT_EQ(bad_speaker, 0u) << "speaker fields holding dialogue, not a bare name";

    // Rotation: the player-visible opening leads the list.
    EXPECT_GE(strings[0].get_object().at("offset").get_int64(),
              static_cast<std::int64_t>(ex::GAME_START_OFFSET));
}

// R3 + R4 against 8 MB of real data, without needing sn.bin.
TEST(Extract, ExtractedJsonSerialisesWithIndentOneAndCrlf) {
    const fs::path p =
        fs::u8path(std::string(CRC_PROJECT_DIR) + "\\script_output\\extracted_text.json");
    if (!fs::exists(p)) GTEST_SKIP() << "extracted_text.json not present";

    const bj::value root = json_parse_file(p.u8string());
    const fs::path tmp = fs::temp_directory_path() / "crc_ut_extracted_roundtrip.json";
    write_file_text(tmp.u8string(), json_pretty(root, 1));

    const Bytes a = read_file(p.u8string());
    const Bytes b = read_file(tmp.u8string());
    fs::remove(tmp);
    EXPECT_EQ(a.size(), b.size());
    EXPECT_TRUE(a == b) << "json_pretty(v,1) + write_file_text does not reproduce the reference";
}
