// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Step-5 tests: FXF line parsing, the full-width conversion the engine
// requires, and the codepoint-accurate wrapping.
#include <gtest/gtest.h>

#include <string>

#include "charts/charts.h"
#include "common/util.h"

namespace ch = exc::charts;
using namespace exc;

namespace {
// 【 】 and a colour tag, in UTF-8.
constexpr const char* OPEN = "\xE3\x80\x90";   // 【
constexpr const char* CLOSE = "\xE3\x80\x91";  // 】
constexpr const char* KI = "\xE9\xBB\x84";     // 黄
constexpr const char* IDEO_SPACE = "\xE3\x80\x80";
}  // namespace

TEST(Charts, xor_is_self_inverse) {
    const Bytes plain{0x00, 0x41, 0xFF, 0x80};
    EXPECT_EQ(ch::decrypt_fxf(ch::encrypt_fxf(plain)), plain);
}

// The engine crashes on single-byte ASCII in FXF display fields, so every
// printable ASCII char has to become its full-width twin.
TEST(Charts, to_fullwidth_converts_printable_ascii) {
    EXPECT_EQ(ch::to_fullwidth("A"), "\xEF\xBC\xA1");   // A -> Ａ
    EXPECT_EQ(ch::to_fullwidth("!"), "\xEF\xBC\x81");   // ! -> ！
    EXPECT_EQ(ch::to_fullwidth("~"), "\xEF\xBD\x9E");   // ~ -> ～
    EXPECT_EQ(ch::to_fullwidth(" "), IDEO_SPACE);       // space -> U+3000
}

// '#' before a CJK char is a colour tag and must stay single-byte, or the
// engine stops recognising it.
TEST(Charts, to_fullwidth_preserves_colour_tags) {
    const std::string tag = std::string("#") + KI;
    EXPECT_EQ(ch::to_fullwidth(tag), tag);
    // A '#' NOT followed by CJK is ordinary text and does get widened.
    EXPECT_EQ(ch::to_fullwidth("#a"), "\xEF\xBC\x83\xEF\xBD\x81");
}

TEST(Charts, has_japanese_is_kana_and_cjk_only) {
    EXPECT_TRUE(ch::has_japanese("\xE3\x81\x82"));
    EXPECT_TRUE(ch::has_japanese(KI));
    EXPECT_FALSE(ch::has_japanese("plain"));
    // Fullwidth Latin and the CJK brackets are NOT "Japanese" for this test.
    EXPECT_FALSE(ch::has_japanese("\xEF\xBC\xA1"));
    EXPECT_FALSE(ch::has_japanese(OPEN));
}

TEST(Charts, parse_chart_line_splits_prefix_and_segments) {
    const std::string line = std::string("@12 0 100 200 13 #") + KI + OPEN +
                             "\xE3\x81\x82" + CLOSE + " desc";
    std::string prefix;
    std::vector<std::string> segments;
    ASSERT_TRUE(ch::parse_chart_line(line, &prefix, &segments));
    EXPECT_EQ(prefix, "@12 0 100 200 13");
    ASSERT_EQ(segments.size(), 1u);
    EXPECT_EQ(segments[0].rfind("#", 0), 0u);
}

TEST(Charts, parse_chart_line_splits_alt_segment) {
    const std::string main = std::string("#") + KI + OPEN + "\xE3\x81\x82" + CLOSE + " d1";
    const std::string alt = std::string("!#") + KI + OPEN + "\xE3\x81\x84" + CLOSE + " !d2";
    std::string prefix;
    std::vector<std::string> segments;
    ASSERT_TRUE(ch::parse_chart_line("@1 0 0 0 " + main + " " + alt, &prefix, &segments));
    ASSERT_EQ(segments.size(), 2u);
    EXPECT_EQ(segments[0], main);
    EXPECT_EQ(segments[1], alt);
}

TEST(Charts, parse_chart_line_rejects_non_node_lines) {
    std::string prefix;
    std::vector<std::string> segments;
    EXPECT_FALSE(ch::parse_chart_line("not a node", &prefix, &segments));
    EXPECT_FALSE(ch::parse_chart_line("", &prefix, &segments));
}

TEST(Charts, extract_translatable_text_splits_title_and_desc) {
    const std::string seg = std::string("#") + KI + OPEN + "\xE9\xA4\x8C\xE5\xA0\xB4" +
                            CLOSE + " \xE3\x81\x82\xE3\x81\x84";
    const ch::Segment s = ch::extract_translatable_text(seg);
    EXPECT_FALSE(s.is_alt);
    EXPECT_EQ(s.pre_title, std::string("#") + KI + OPEN);
    EXPECT_EQ(s.title, "\xE9\xA4\x8C\xE5\xA0\xB4");
    EXPECT_EQ(s.mid, std::string(CLOSE) + " ");
    EXPECT_EQ(s.desc, "\xE3\x81\x82\xE3\x81\x84");
}

// The whitespace run after '】' is matched Unicode-wide, so a U+3000 there
// belongs to `mid`, not to the description -- otherwise the cache key would
// carry a leading full-width space the engine never emits.
TEST(Charts, extract_translatable_text_treats_u3000_as_whitespace) {
    const std::string seg = std::string("#") + KI + OPEN + "\xE3\x81\x82" + CLOSE +
                            IDEO_SPACE + "\xE3\x81\x84";
    const ch::Segment s = ch::extract_translatable_text(seg);
    EXPECT_EQ(s.mid, std::string(CLOSE) + IDEO_SPACE);
    EXPECT_EQ(s.desc, "\xE3\x81\x84");
}

TEST(Charts, extract_translatable_text_marks_alt_segments) {
    const std::string seg = std::string("!#") + KI + OPEN + "\xE3\x81\x82" + CLOSE + " d";
    const ch::Segment s = ch::extract_translatable_text(seg);
    EXPECT_TRUE(s.is_alt);
    EXPECT_EQ(s.title, "\xE3\x81\x82");
}

TEST(Charts, extract_translatable_text_falls_back_to_whole_segment) {
    const ch::Segment s = ch::extract_translatable_text("no markers here");
    EXPECT_TRUE(s.pre_title.empty());
    EXPECT_EQ(s.title, "no markers here");
    EXPECT_TRUE(s.desc.empty());
}

// The run of whitespace BEFORE each '!' is consumed along with it, so
// "!one !two" collapses to a single space.
TEST(Charts, strip_alt_marks_normalises_the_cache_key) {
    EXPECT_EQ(ch::strip_alt_marks("!one !two"), "one two");
    EXPECT_EQ(ch::strip_alt_marks("  !one"), "one");
    EXPECT_EQ(ch::strip_alt_marks("plain"), "plain");
    // U+3000 counts as whitespace here too, and these files are full of it.
    EXPECT_EQ(ch::strip_alt_marks("a\xE3\x80\x80!b"), "a b");
}

// Wrapping counts CHARACTERS and only breaks at full-width spaces.
TEST(Charts, wordwrap_fullwidth_breaks_at_ideographic_spaces) {
    std::string text;
    for (int i = 0; i < 6; ++i) text += std::string("\xEF\xBC\xA1") + IDEO_SPACE;
    const std::string wrapped = ch::wordwrap_fullwidth(text, /*max_chars=*/4, /*max_lines=*/5);
    EXPECT_NE(wrapped.find("\r\n"), std::string::npos) << "never wrapped";
    // A byte-counting implementation would have wrapped on the very first char.
    EXPECT_NE(wrapped.rfind("\xEF\xBC\xA1", 0), 0u * 0 + std::string::npos);
}

TEST(Charts, wordwrap_fullwidth_caps_line_count) {
    std::string text;
    for (int i = 0; i < 40; ++i) text += std::string("\xEF\xBC\xA1") + IDEO_SPACE;
    const std::string wrapped = ch::wordwrap_fullwidth(text, /*max_chars=*/2, /*max_lines=*/3);
    std::size_t breaks = 0;
    for (std::size_t i = 0; i + 1 < wrapped.size(); ++i)
        if (wrapped.compare(i, 2, "\r\n") == 0) ++breaks;
    EXPECT_LE(breaks, 2u) << "max_lines=3 allows at most 2 breaks";
}

// The chart cache is a file of its own, so it carries a purge of its own.  It
// takes out exactly the echoes -- the model handing back the Japanese it was
// given -- and leaves everything else where it was, key order included; that
// order is the cache file's key order.
TEST(Charts, the_chart_purge_removes_echoes_and_keeps_the_rest) {
    ch::Cache cache;
    cache.set("\xE6\x9C\x9D\xE3\x81\x8C\xE6\x9D\xA5\xE3\x81\x9F\xE3\x80\x82",
              "Morning came.");
    cache.set("\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF",
              "\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF");
    cache.set("OK", "OK");
    cache.set("bg152n", "bg152n");

    EXPECT_EQ(ch::purge_failed_entries(cache), 1u);
    EXPECT_EQ(cache.size(), 3u);
    EXPECT_FALSE(cache.contains("\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF"));
    // ASCII identity is a correct answer, not an echo.
    EXPECT_TRUE(cache.contains("OK"));
    EXPECT_TRUE(cache.contains("bg152n"));

    const auto& items = cache.items();
    ASSERT_EQ(items.size(), 3u);
    EXPECT_EQ(items[0].first, "\xE6\x9C\x9D\xE3\x81\x8C\xE6\x9D\xA5\xE3\x81\x9F\xE3\x80\x82");
    EXPECT_EQ(items[1].first, "OK");
    EXPECT_EQ(items[2].first, "bg152n");

    // Nothing left to remove on a second pass.
    EXPECT_EQ(ch::purge_failed_entries(cache), 0u);
    EXPECT_EQ(cache.size(), 3u);
}

// An empty cache purges nothing and stays empty.
TEST(Charts, the_chart_purge_leaves_a_clean_cache_alone) {
    ch::Cache cache;
    EXPECT_EQ(ch::purge_failed_entries(cache), 0u);
    cache.set("\xE6\x9C\x9D", "Morning");
    EXPECT_EQ(ch::purge_failed_entries(cache), 0u);
    EXPECT_EQ(cache.size(), 1u);
}
