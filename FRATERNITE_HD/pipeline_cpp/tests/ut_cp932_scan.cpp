// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// The CP932 Japanese-run scanner: run boundaries, size limits, edge trimming.
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "common/util.h"
#include "yuris/cp932_scan.h"

using namespace frat;
using namespace frat::yuris;

namespace {

Bytes cp932(const std::string& utf8) { return utf8_to_cp932_replace(utf8); }

Bytes cat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const auto& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

Bytes raw(std::initializer_list<std::uint8_t> b) { return Bytes(b); }

}  // namespace

TEST(Cp932Scan, Scan_RunBoundaries_HalfwidthKanaBreaksRun) {
    // 0xA1-0xDF are valid CP932 characters and valid TRAIL bytes, but the run
    // scanner accepts neither them nor any other lone byte >= 0x7F, so a
    // halfwidth-kana byte splits one run into two.
    const Bytes data = cat({cp932("あいうえお"), raw({0xB1}), cp932("かきくけこ")});
    const auto runs = scan_cp932_jp(data);
    ASSERT_EQ(runs.size(), 2u);
    EXPECT_EQ(runs[0].second, "あいうえお");
    EXPECT_EQ(runs[1].second, "かきくけこ");
    EXPECT_EQ(runs[1].first, 11u);  // 10 bytes of kana + the 1 rejected byte
}

TEST(Cp932Scan, Scan_MinRun4_MaxRun16384) {
    // A 3-byte run ("aあ") is below min_run even though it has a double byte.
    EXPECT_TRUE(scan_cp932_jp(cat({raw({0x00}), cp932("aあ"), raw({0x00})})).empty());
    // Exactly 4 is kept.
    EXPECT_EQ(scan_cp932_jp(cat({raw({0x00}), cp932("ああ"), raw({0x00})})).size(), 1u);

    std::string long_jp;
    for (int i = 0; i < 8192; ++i) long_jp += "あ";  // 16,384 CP932 bytes
    EXPECT_EQ(scan_cp932_jp(cp932(long_jp)).size(), 1u);
    EXPECT_TRUE(scan_cp932_jp(cp932(long_jp + "あ")).empty());
}

TEST(Cp932Scan, Scan_DecodeFailure_SkipsWholeRunWithoutRewinding) {
    // 0x85 0x40 is structurally valid CP932 but unmapped, so the strict decode
    // of the whole run fails.  The ENTIRE run is then skipped and scanning
    // resumes at the already-advanced cursor -- the deliberate inverse of
    // CLAUDE.md section 9.  Sliding one byte would recover the kana instead,
    // and would change every extracted key.
    const Bytes doomed = cat({cp932("あいうえお"), raw({0x85, 0x40}), cp932("かきくけこ")});
    EXPECT_TRUE(scan_cp932_jp(doomed).empty());

    // Scanning resumes past the run, not at start+1: a clean run AFTER a
    // terminator byte is still found.
    const Bytes resumed = cat({doomed, raw({0x00}), cp932("さしすせそ")});
    const auto runs = scan_cp932_jp(resumed);
    ASSERT_EQ(runs.size(), 1u);
    EXPECT_EQ(runs[0].second, "さしすせそ");
}

TEST(Cp932Scan, TrimAsciiEdges_DropsBracketsAndAsciiOpcodeBytes) {
    // U+300C / U+300D sit outside 0x3040-0x9FFF, so a leading 「 and trailing
    // 」 are trimmed off a run -- which is why only split_into_messages can
    // produce a "dialogue" piece.
    const Bytes data = cat({raw({0x00}), Bytes{'a', 'b'}, cp932("「あい」"), Bytes{'c', 'd'},
                            raw({0x00})});
    const auto runs = scan_cp932_jp(data);
    ASSERT_EQ(runs.size(), 1u);
    EXPECT_EQ(runs[0].second, "あい");

    EXPECT_EQ(trim_ascii_edges("abc"), "");
    EXPECT_EQ(trim_ascii_edges("ab\xE3\x81\x82" "cd"), "\xE3\x81\x82");
}

TEST(Cp932Scan, StripChars_KeepsU3000_UnlikeTheWhitespaceStrip) {
    const std::string kEdge(" \t\r\n\0", 5);
    const std::string ideo = "\xE3\x80\x80";  // U+3000
    const std::string s = ideo + "abc" + ideo;
    EXPECT_EQ(strip_chars(s, kEdge), s);  // listed characters only: U+3000 stays
    EXPECT_EQ(trim(s), "abc");        // whole-whitespace strip removes it
    EXPECT_EQ(strip_chars(std::string("\0 x \t", 5), kEdge), "x");
}

TEST(Cp932Scan, HasJapaneseRun_AcceptsCjkSymbolsAndFullwidth) {
    EXPECT_TRUE(has_japanese_run("\xE3\x80\x82"));  // U+3002 。
    EXPECT_TRUE(has_japanese_run("\xEF\xBC\xA1"));  // U+FF21 Ａ
    EXPECT_FALSE(has_japanese_run("plain ascii"));
}
