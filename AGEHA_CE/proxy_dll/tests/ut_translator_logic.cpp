// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Unit tests for proxy_dll/src/translator_logic.{h,cpp}: BOM detection,
// backslash-escape expansion, and CP932 TSV buffer parsing.
#include "translator_logic.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>

using translator_logic::ParseTsvBuffer;
using translator_logic::TranslationMap;
using translator_logic::UnescapeInPlace;
using translator_logic::Utf8BomLen;

namespace {

constexpr std::size_t kExpectedBomLen = 3;

}  // namespace

TEST(TranslatorLogic, utf8_bom_detection) {
    const char withBom[] = "\xEF\xBB\xBFhello\tHello\n";
    EXPECT_EQ(Utf8BomLen(withBom, sizeof(withBom) - 1), kExpectedBomLen);

    const char withoutBom[] = "hello\tHello\n";
    EXPECT_EQ(Utf8BomLen(withoutBom, sizeof(withoutBom) - 1), 0u);
    EXPECT_EQ(Utf8BomLen(nullptr, 0), 0u);
}

TEST(TranslatorLogic, unescape_basic_escapes) {
    std::string s = "a\\nb\\tc\\\\d\\re";
    UnescapeInPlace(s);
    EXPECT_EQ(s, "a\nb\tc\\d\re");
}

TEST(TranslatorLogic, unescape_preserves_unknown_and_trailing_backslashes) {
    std::string plain = "plain ascii";
    UnescapeInPlace(plain);
    EXPECT_EQ(plain, "plain ascii");

    std::string unknown = "a\\qb";
    UnescapeInPlace(unknown);
    EXPECT_EQ(unknown, "a\\qb");

    std::string trailing = "trailing\\";
    UnescapeInPlace(trailing);
    EXPECT_EQ(trailing, "trailing\\");
}

TEST(TranslatorLogic, parse_tsv_buffer_handles_empty_and_valid_rows) {
    TranslationMap emptyMap;
    EXPECT_EQ(ParseTsvBuffer("", 0, emptyMap), 0);
    EXPECT_TRUE(emptyMap.empty());

    TranslationMap map;
    const char tsv[] =
        "\x83\x41\x83\x51\x83\x6E\\r\\n\x81\x75\x82\xA0\x81\x76\tAgeha: \"Ah\"\\r\\n\n"
        "second\tSecond\r\n"
        "malformed_no_tab\n"
        "empty_val\t\n"
        "\tempty_key\n";
    const int count = ParseTsvBuffer(tsv, sizeof(tsv) - 1, map);
    EXPECT_EQ(count, 2);
    EXPECT_EQ(map["\x83\x41\x83\x51\x83\x6E\r\n\x81\x75\x82\xA0\x81\x76"],
              "Ageha: \"Ah\"\r\n");
    EXPECT_EQ(map["second"], "Second");
}
