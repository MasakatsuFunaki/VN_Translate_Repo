// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "storage/json_format_style.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace spot_checker {
namespace {

TEST(JsonFormatStyle, DetectsAndRoundTripsIndentLineEndingAndTrailingNewlineMatrix) {
    struct StyleCase {
        int indent;
        bool use_crlf;
        bool trailing_nl;
    };
    constexpr StyleCase kMatrix[] = {
        {1, false, false}, {1, false, true}, {1, true, false}, {1, true, true},
        {2, false, false}, {2, false, true}, {2, true, false}, {2, true, true},
        {4, false, false}, {4, false, true}, {4, true, false}, {4, true, true},
    };

    for (const auto& tc : kMatrix) {
        const std::string nl = tc.use_crlf ? "\r\n" : "\n";
        const std::string pad(static_cast<std::size_t>(tc.indent), ' ');
        const std::string expected_json =
            "{" + nl + pad + "\"おはよう\": \"Good morning.\"," + nl + pad +
            "\"さようなら\": \"Goodbye.\"" + nl + "}" +
            (tc.trailing_nl ? nl : "");

        SCOPED_TRACE("indent=" + std::to_string(tc.indent) +
                     " crlf=" + std::to_string(tc.use_crlf) +
                     " trail=" + std::to_string(tc.trailing_nl));

        const JsonFormatStyle detected = detect_json_format_style(expected_json);
        EXPECT_EQ(detected.indent_spaces, tc.indent);
        EXPECT_EQ(detected.use_crlf, tc.use_crlf);
        EXPECT_EQ(detected.trailing_newline, tc.trailing_nl);

        const Result<OrderedStringMap> parsed =
            parse_ordered_json_string_map(expected_json, "matrix_cache.json");
        ASSERT_TRUE(parsed.is_ok()) << parsed.error_message();
        ASSERT_EQ(parsed.value().size(), 2U);

        const std::string serialized =
            serialize_ordered_json_string_map(parsed.value(), detected);
        EXPECT_EQ(serialized, expected_json);
    }

    constexpr std::string_view kCompactJson = "{\"おはよう\":\"Good morning.\"}";
    const JsonFormatStyle compact_style = detect_json_format_style(kCompactJson);
    EXPECT_EQ(compact_style.indent_spaces, 0);
    EXPECT_FALSE(compact_style.use_crlf);
    EXPECT_FALSE(compact_style.trailing_newline);
}

TEST(JsonFormatStyle, PreservesInsertionOrderAcrossInPlaceUpdatesAndEscapes) {
    OrderedStringMap map;
    EXPECT_EQ(map.size(), 0U);

    map.set("ん", "N");
    map.set("あ", "Initial A");
    map.set("か", "Ka");
    map.set("さ", "Sa");
    map.set("あ", "Updated \"A\"\nLine 2");
    map.set("た", "Ta");

    ASSERT_EQ(map.size(), 5U);
    const std::vector<std::string> expected_keys{"ん", "あ", "か", "さ", "た"};
    ASSERT_EQ(map.keys().size(), expected_keys.size());
    EXPECT_EQ(map.keys(), expected_keys);

    EXPECT_TRUE(map.contains("あ"));
    EXPECT_FALSE(map.contains("missing"));
    ASSERT_NE(map.find("あ"), nullptr);
    EXPECT_EQ(*map.find("あ"), "Updated \"A\"\nLine 2");
    EXPECT_EQ(map.find("missing"), nullptr);

    JsonFormatStyle style;
    style.indent_spaces = 2;
    style.use_crlf = false;
    style.trailing_newline = true;

    const std::string serialized = serialize_ordered_json_string_map(map, style);
    const Result<OrderedStringMap> reparsed =
        parse_ordered_json_string_map(serialized, "roundtrip.json");
    ASSERT_TRUE(reparsed.is_ok()) << reparsed.error_message();
    ASSERT_EQ(reparsed.value().keys().size(), expected_keys.size());
    EXPECT_EQ(reparsed.value().keys(), expected_keys);
    ASSERT_NE(reparsed.value().find("あ"), nullptr);
    EXPECT_EQ(*reparsed.value().find("あ"), "Updated \"A\"\nLine 2");
}

TEST(JsonFormatStyle, RejectsCorruptAndNonObjectJsonWithSourceLabel) {
    constexpr std::string_view kInvalidInputs[] = {
        "",
        "{\"おはよう\": \"Good morning.\"",
        "[\"not\", \"an\", \"object\"]",
        "\"scalar_root\"",
        "12345",
    };

    for (const std::string_view bad : kInvalidInputs) {
        SCOPED_TRACE(bad);
        const Result<OrderedStringMap> res =
            parse_ordered_json_string_map(bad, "broken_cache.json");
        EXPECT_FALSE(res.is_ok());
        EXPECT_EQ(res.error_code(), ErrorCode::CorruptJson);
        EXPECT_NE(res.error_message().find("broken_cache.json"),
                  std::string::npos);
    }
}

}  // namespace
}  // namespace spot_checker
