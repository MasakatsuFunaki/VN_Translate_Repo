// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Parsing / classification helpers of the narrative-CG step.
#include <gtest/gtest.h>

#include <string>

#include <boost/json.hpp>

#include "cg/cg_pipeline.h"

namespace bj = boost::json;
using namespace mgi::cg::detail;

// The exact regression _parse_json_array's docstring describes: the old greedy
// regex matched from the first '[' to the LAST ']' in the reply.
TEST(CgParse, parse_json_array_stops_at_balance) {
    const std::string s =
        R"([{"a":1}] and then some prose with a stray ] bracket)";
    const auto t = balanced_json_array_text(s);
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(*t, R"([{"a":1}])");
    EXPECT_EQ(bj::parse(*t).get_array().size(), 1u);
}

TEST(CgParse, parse_json_array_ignores_brackets_in_strings) {
    const std::string s = R"(prose [{"text_en":"a ] and \" bracket","bbox":[1,2,3,4]}] tail ])";
    // Hoisted out of the EXPECT_EQ: MSVC's legacy preprocessor re-escapes a raw
    // string passed as a macro argument, so `\"` inside one fails to compile.
    const std::string expected = R"([{"text_en":"a ] and \" bracket","bbox":[1,2,3,4]}])";
    const auto t = balanced_json_array_text(s);
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(*t, expected);
    const bj::array a = bj::parse(*t).get_array();
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(a[0].get_object().at("bbox").get_array().size(), 4u);
}

TEST(CgParse, parse_json_array_returns_none_when_unbalanced) {
    EXPECT_FALSE(balanced_json_array_text("no array here").has_value());
    EXPECT_FALSE(balanced_json_array_text("[1, 2, 3").has_value());
}

// re.search(r'\{.*?\}', DOTALL): first '{' to the FIRST '}' after it.
TEST(CgParse, first_json_object_is_non_greedy) {
    const std::string s =
        R"(prose {"has_narrative_text": true, "confidence": 0.9} more {junk})";
    const auto t = first_json_object_text(s);
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(*t, R"({"has_narrative_text": true, "confidence": 0.9})");
    EXPECT_FALSE(first_json_object_text("no braces").has_value());
    EXPECT_FALSE(first_json_object_text("{ unterminated").has_value());
}

TEST(CgParse, detect_gate_at_exactly_0_7) {
    const auto hit = [](const char* json) {
        return detect_hit(bj::parse(json).get_object());
    };
    EXPECT_TRUE(hit(R"({"has_narrative_text": true, "confidence": 0.7})"));
    EXPECT_FALSE(hit(R"({"has_narrative_text": true, "confidence": 0.699})"));
    EXPECT_FALSE(hit(R"({"has_narrative_text": false, "confidence": 0.99})"));
    EXPECT_FALSE(hit(R"({"has_narrative_text": true})"));  // missing -> 0
    EXPECT_TRUE(hit(R"({"has_narrative_text": true, "confidence": 1})"));  // int
    // Loose truthiness, not a strict `== true`.
    EXPECT_TRUE(hit(R"({"has_narrative_text": "yes", "confidence": 0.8})"));
    EXPECT_FALSE(hit(R"({"has_narrative_text": "", "confidence": 0.8})"));
    // A non-numeric confidence throws; the caller logs it as a detect error.
    EXPECT_THROW(hit(R"({"has_narrative_text": true, "confidence": "high"})"),
                 std::exception);
}

TEST(CgParse, safe_name_replaces_exactly_the_invalid_set) {
    EXPECT_EQ(safe_name("ev01a"), "ev01a");
    EXPECT_EQ(safe_name("a\"b<c>d|e:f*g?h\\i/j"), "a_b_c_d_e_f_g_h_i_j");
    EXPECT_EQ(safe_name(std::string("x\x01y\x1f", 4)), "x_y_");
    // Untouched: separators and punctuation the set does not list.
    EXPECT_EQ(safe_name("a.b-c_d+e f"), "a.b-c_d+e f");
    // Japanese passes through: every invalid codepoint is < 0x80, and UTF-8
    // continuation bytes are >= 0x80.
    EXPECT_EQ(safe_name("\xE6\x97\xA5\xE6\x9C\xAC"), "\xE6\x97\xA5\xE6\x9C\xAC");
}

TEST(CgParse, ext_from_type_tag) {
    EXPECT_EQ(ext_for_type_tag("PNG"), ".png");
    EXPECT_EQ(ext_for_type_tag("BMP"), ".bmp");
    EXPECT_EQ(ext_for_type_tag("JPG"), ".jpg");
    EXPECT_EQ(ext_for_type_tag("JPEG"), ".jpg");
    EXPECT_EQ(ext_for_type_tag("GIF"), "");
    EXPECT_EQ(ext_for_type_tag("BIN"), "");
    EXPECT_EQ(ext_for_type_tag(""), "");
    // Substring match, not equality.
    EXPECT_EQ(ext_for_type_tag("MYPNGTHING"), ".png");
}

TEST(CgParse, to_int_truncates_toward_zero) {
    EXPECT_EQ(to_int(bj::value(130)), 130);
    EXPECT_EQ(to_int(bj::value(130.9)), 130);
    EXPECT_EQ(to_int(bj::value(-130.9)), -130);
    EXPECT_EQ(to_int(bj::value(true)), 1);
    EXPECT_THROW(to_int(bj::value("abc")), std::exception);
    EXPECT_EQ(to_int(bj::value(" 12 ")), 12);
    EXPECT_THROW(to_int(bj::value(nullptr)), std::exception);
}
