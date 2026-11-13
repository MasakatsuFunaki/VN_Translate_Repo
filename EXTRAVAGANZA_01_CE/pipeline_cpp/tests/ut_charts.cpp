// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Unit tests for chart translation helpers: FXF line parsing, full-width
// conversion required by the engine, and codepoint-accurate wrapping.
#include "charts/charts.h"
#include "common/util.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

namespace ch = exc::charts;
using namespace exc;

namespace {

constexpr const char* kOpenLenticular = "【";
constexpr const char* kCloseLenticular = "】";
constexpr const char* kYellowTagChar = "黄";
constexpr const char* kIdeoSpace = "　";

constexpr int kShortWrapTokenCount = 6;
constexpr int kShortWrapMaxChars = 4;
constexpr int kShortWrapMaxLines = 5;
constexpr int kLongWrapTokenCount = 40;
constexpr int kLongWrapMaxChars = 2;
constexpr int kLongWrapMaxLines = 3;
constexpr std::size_t kMaxAllowedLineBreaks = 2;

}  // namespace

TEST(Charts, xor_is_self_inverse) {
    const Bytes plain{0x00, 0x41, 0xFF, 0x80};
    EXPECT_EQ(ch::decrypt_fxf(ch::encrypt_fxf(plain)), plain);
}

// The engine crashes on single-byte ASCII in FXF display fields, so every
// printable ASCII char has to become its full-width twin.
TEST(Charts, to_fullwidth_converts_printable_ascii) {
    EXPECT_EQ(ch::to_fullwidth("A"), "Ａ");
    EXPECT_EQ(ch::to_fullwidth("!"), "！");
    EXPECT_EQ(ch::to_fullwidth("~"), "～");
    EXPECT_EQ(ch::to_fullwidth(" "), kIdeoSpace);
}

// '#' before a CJK char is a colour tag and must stay single-byte, or the
// engine stops recognising it.
TEST(Charts, to_fullwidth_preserves_colour_tags) {
    const std::string tag = std::string("#") + kYellowTagChar;
    EXPECT_EQ(ch::to_fullwidth(tag), tag);
    EXPECT_EQ(ch::to_fullwidth("#a"), "＃ａ");
}

TEST(Charts, has_japanese_is_kana_and_cjk_only) {
    EXPECT_TRUE(ch::has_japanese("あ"));
    EXPECT_TRUE(ch::has_japanese(kYellowTagChar));
    EXPECT_FALSE(ch::has_japanese("plain"));
    EXPECT_FALSE(ch::has_japanese("Ａ"));
    EXPECT_FALSE(ch::has_japanese(kOpenLenticular));
}

TEST(Charts, parse_chart_line_splits_prefix_and_segments) {
    const std::string line = std::string("@12 0 100 200 13 #") + kYellowTagChar +
                             kOpenLenticular + "あ" + kCloseLenticular + " desc";
    std::string prefix;
    std::vector<std::string> segments;
    ASSERT_TRUE(ch::parse_chart_line(line, &prefix, &segments));
    EXPECT_EQ(prefix, "@12 0 100 200 13");
    ASSERT_EQ(segments.size(), 1u);
    EXPECT_EQ(segments[0].rfind("#", 0), 0u);
}

TEST(Charts, parse_chart_line_splits_alt_segment) {
    const std::string main_seg = std::string("#") + kYellowTagChar + kOpenLenticular +
                                 "あ" + kCloseLenticular + " d1";
    const std::string alt_seg = std::string("!#") + kYellowTagChar + kOpenLenticular +
                                "い" + kCloseLenticular + " !d2";
    std::string prefix;
    std::vector<std::string> segments;
    ASSERT_TRUE(
        ch::parse_chart_line("@1 0 0 0 " + main_seg + " " + alt_seg, &prefix, &segments));
    ASSERT_EQ(segments.size(), 2u);
    EXPECT_EQ(segments[0], main_seg);
    EXPECT_EQ(segments[1], alt_seg);
}

TEST(Charts, parse_chart_line_rejects_non_node_lines) {
    std::string prefix;
    std::vector<std::string> segments;
    EXPECT_FALSE(ch::parse_chart_line("not a node", &prefix, &segments));
    EXPECT_FALSE(ch::parse_chart_line("", &prefix, &segments));
}

TEST(Charts, extract_translatable_text_splits_title_and_desc) {
    const std::string seg = std::string("#") + kYellowTagChar + kOpenLenticular + "餌場" +
                            kCloseLenticular + " あい";
    const ch::Segment s = ch::extract_translatable_text(seg);
    EXPECT_FALSE(s.is_alt);
    EXPECT_EQ(s.pre_title, std::string("#") + kYellowTagChar + kOpenLenticular);
    EXPECT_EQ(s.title, "餌場");
    EXPECT_EQ(s.mid, std::string(kCloseLenticular) + " ");
    EXPECT_EQ(s.desc, "あい");
}

// The whitespace run after '】' is matched Unicode-wide, so a U+3000 there
// belongs to `mid`, not to the description -- otherwise the cache key would
// carry a leading full-width space the engine never emits.
TEST(Charts, extract_translatable_text_treats_u3000_as_whitespace) {
    const std::string seg = std::string("#") + kYellowTagChar + kOpenLenticular + "あ" +
                            kCloseLenticular + kIdeoSpace + "い";
    const ch::Segment s = ch::extract_translatable_text(seg);
    EXPECT_EQ(s.mid, std::string(kCloseLenticular) + kIdeoSpace);
    EXPECT_EQ(s.desc, "い");
}

TEST(Charts, extract_translatable_text_marks_alt_segments) {
    const std::string seg = std::string("!#") + kYellowTagChar + kOpenLenticular + "あ" +
                            kCloseLenticular + " d";
    const ch::Segment s = ch::extract_translatable_text(seg);
    EXPECT_TRUE(s.is_alt);
    EXPECT_EQ(s.title, "あ");
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
    EXPECT_EQ(ch::strip_alt_marks("a　!b"), "a b");
}

// Wrapping counts CHARACTERS and only breaks at full-width spaces.
TEST(Charts, wordwrap_fullwidth_breaks_at_ideographic_spaces) {
    std::string text;
    for (int i = 0; i < kShortWrapTokenCount; ++i) {
        text += std::string("Ａ") + kIdeoSpace;
    }
    const std::string wrapped =
        ch::wordwrap_fullwidth(text, kShortWrapMaxChars, kShortWrapMaxLines);
    EXPECT_NE(wrapped.find("\r\n"), std::string::npos) << "never wrapped";
    EXPECT_NE(wrapped.rfind("Ａ", 0), std::string::npos);
}

TEST(Charts, wordwrap_fullwidth_caps_line_count) {
    std::string text;
    for (int i = 0; i < kLongWrapTokenCount; ++i) {
        text += std::string("Ａ") + kIdeoSpace;
    }
    const std::string wrapped =
        ch::wordwrap_fullwidth(text, kLongWrapMaxChars, kLongWrapMaxLines);
    std::size_t breaks = 0;
    for (std::size_t i = 0; i + 1 < wrapped.size(); ++i) {
        if (wrapped.compare(i, 2, "\r\n") == 0) {
            ++breaks;
        }
    }
    EXPECT_LE(breaks, kMaxAllowedLineBreaks) << "max_lines=3 allows at most 2 breaks";
}

TEST(Charts, the_chart_purge_removes_echoes_and_keeps_the_rest) {
    ch::Cache cache;
    cache.set("朝が来た。", "Morning came.");
    cache.set("こんにちは", "こんにちは");
    cache.set("OK", "OK");
    cache.set("bg152n", "bg152n");

    EXPECT_EQ(ch::purge_failed_entries(cache), 1u);
    EXPECT_EQ(cache.size(), 3u);
    EXPECT_FALSE(cache.contains("こんにちは"));
    EXPECT_TRUE(cache.contains("OK"));
    EXPECT_TRUE(cache.contains("bg152n"));

    const auto& items = cache.items();
    ASSERT_EQ(items.size(), 3u);
    EXPECT_EQ(items[0].first, "朝が来た。");
    EXPECT_EQ(items[1].first, "OK");
    EXPECT_EQ(items[2].first, "bg152n");

    EXPECT_EQ(ch::purge_failed_entries(cache), 0u);
    EXPECT_EQ(cache.size(), 3u);
}

TEST(Charts, the_chart_purge_leaves_a_clean_cache_alone) {
    ch::Cache cache;
    EXPECT_EQ(ch::purge_failed_entries(cache), 0u);
    cache.set("朝", "Morning");
    EXPECT_EQ(ch::purge_failed_entries(cache), 0u);
    EXPECT_EQ(cache.size(), 1u);
}
