// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Pure standalone unit tests for proxy_dll translator_logic (`tr::load_tsv`,
// `tr::load_tsv_file`, `tr::lookup`, and `tr::has_japanese_utf8`).
#include "translator_logic.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>

#include <gtest/gtest.h>

namespace {

namespace fs = std::filesystem;

constexpr std::uint8_t kUtf8BomBytes[] = {0xEFU, 0xBBU, 0xBFU};
constexpr std::uint8_t kTruncatedThreeByteHiragana[] = {0xE3U, 0x81U};
constexpr std::uint8_t kInvalidContinuationThreeByte[] = {0xE3U, 0x20U, 0x82U};
constexpr std::uint8_t kInvalidLeadByteBeforeKanji[] = {0xFFU, 0xE7U, 0x9BU, 0xB4U};

constexpr std::size_t kTwoEntries = 2;
constexpr std::size_t kThreeEntries = 3;

std::string utf8_bom_prefix() {
    return std::string(reinterpret_cast<const char*>(kUtf8BomBytes),
                       sizeof(kUtf8BomBytes));
}

template <std::size_t N>
std::string bytes_to_string(const std::uint8_t (&raw)[N]) {
    return std::string(reinterpret_cast<const char*>(raw), N);
}

struct ScopedTempDir {
    explicit ScopedTempDir(const std::string& name)
        : path(fs::temp_directory_path() / fs::u8path(name)) {
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    fs::path path;
};

}  // namespace

TEST(TranslatorLogicUt, LoadTsvEmptyInputInsertsNothing) {
    std::unordered_map<std::string, std::string> table;
    EXPECT_EQ(tr::load_tsv("", table), 0U);
    EXPECT_TRUE(table.empty());
}

TEST(TranslatorLogicUt, LoadTsvParsesLiteralJapaneseRowsAndLookupHitsAndMisses) {
    std::unordered_map<std::string, std::string> table;
    const std::string payload =
        "直哉\tNaoya\n"
        "「おはよう」\t\"Good morning.\"\n"
        "　桜が舞う。\t　The cherry blossoms dance.";

    EXPECT_EQ(tr::load_tsv(payload, table), kThreeEntries);
    EXPECT_EQ(table.size(), kThreeEntries);

    std::string out = "untouched";
    ASSERT_TRUE(tr::lookup(table, "直哉", out));
    EXPECT_EQ(out, "Naoya");

    ASSERT_TRUE(tr::lookup(table, "「おはよう」", out));
    EXPECT_EQ(out, "\"Good morning.\"");

    ASSERT_TRUE(tr::lookup(table, "　桜が舞う。", out));
    EXPECT_EQ(out, "　The cherry blossoms dance.");

    out = "sentinel";
    EXPECT_FALSE(tr::lookup(table, "心鈴", out));
    EXPECT_EQ(out, "sentinel");
}

TEST(TranslatorLogicUt, LoadTsvStripsUtf8BomAndCrlfLineEndings) {
    std::unordered_map<std::string, std::string> table;
    const std::string payload =
        utf8_bom_prefix() +
        "直哉\tNaoya\r\n"
        "心鈴\tMisuzu\r\n";

    EXPECT_EQ(tr::load_tsv(payload, table), kTwoEntries);

    std::string out;
    ASSERT_TRUE(tr::lookup(table, "直哉", out));
    EXPECT_EQ(out, "Naoya");
    ASSERT_TRUE(tr::lookup(table, "心鈴", out));
    EXPECT_EQ(out, "Misuzu");
}

TEST(TranslatorLogicUt, LoadTsvSkipsCommentsBlankAndMalformedLines) {
    std::unordered_map<std::string, std::string> table;
    const std::string payload =
        "# comment header\n"
        "\n"
        "\r\n"
        "line_without_tab\n"
        "\tempty_key\n"
        "empty_value\t\n"
        "第一章\tChapter 1\n"
        "# trailing comment\n";

    EXPECT_EQ(tr::load_tsv(payload, table), 1U);
    EXPECT_EQ(table.size(), 1U);

    std::string out;
    ASSERT_TRUE(tr::lookup(table, "第一章", out));
    EXPECT_EQ(out, "Chapter 1");
}

TEST(TranslatorLogicUt, LoadTsvUnescapesBackslashSequencesInKeysAndValues) {
    std::unordered_map<std::string, std::string> table;
    const std::string payload =
        "一行目\\n二行目\tFirst line\\nSecond line\n"
        "列\\t区切り\\\\と\\r復帰\tCol\\tSep\\\\And\\rCr\n"
        "未知\\qエスケープ\\\tUnknown\\qEscape\\\n";

    EXPECT_EQ(tr::load_tsv(payload, table), kThreeEntries);

    std::string out;
    ASSERT_TRUE(tr::lookup(table, "一行目\n二行目", out));
    EXPECT_EQ(out, "First line\nSecond line");

    ASSERT_TRUE(tr::lookup(table, "列\t区切り\\と\r復帰", out));
    EXPECT_EQ(out, "Col\tSep\\And\rCr");

    ASSERT_TRUE(tr::lookup(table, "未知\\qエスケープ\\", out));
    EXPECT_EQ(out, "Unknown\\qEscape\\");
}

TEST(TranslatorLogicUt, LoadTsvKeepsFirstOccurrenceOnDuplicateKeys) {
    std::unordered_map<std::string, std::string> table;
    const std::string payload =
        "直哉\tNaoya\n"
        "直哉\tOverwritten\n";

    EXPECT_EQ(tr::load_tsv(payload, table), 1U);
    std::string out;
    ASSERT_TRUE(tr::lookup(table, "直哉", out));
    EXPECT_EQ(out, "Naoya");
}

TEST(TranslatorLogicUt, LoadTsvFileReadsUnicodePathAndRejectsMissingFile) {
    ScopedTempDir dir("snt_proxy_ut_サクラノ刻");
    const fs::path tsv_file = dir.path / fs::u8path("translations_テスト.tsv");
    {
        std::ofstream out_stream(tsv_file, std::ios::binary);
        const std::string content =
            utf8_bom_prefix() +
            "直哉\tNaoya\r\n"
            "「おはよう」\t\"Good morning.\"\r\n";
        out_stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    std::unordered_map<std::string, std::string> table;
    EXPECT_EQ(tr::load_tsv_file(tsv_file.u8string(), table), kTwoEntries);

    std::string resolved;
    ASSERT_TRUE(tr::lookup(table, "「おはよう」", resolved));
    EXPECT_EQ(resolved, "\"Good morning.\"");

    const fs::path missing_file = dir.path / "does_not_exist.tsv";
    EXPECT_EQ(tr::load_tsv_file(missing_file.u8string(), table), SIZE_MAX);
    EXPECT_EQ(table.size(), kTwoEntries);
}

TEST(TranslatorLogicUt, HasJapaneseUtf8ClassifiesValidAndInvalidInputs) {
    EXPECT_TRUE(tr::has_japanese_utf8("あ"));
    EXPECT_TRUE(tr::has_japanese_utf8("ア"));
    EXPECT_TRUE(tr::has_japanese_utf8("直哉"));
    EXPECT_TRUE(tr::has_japanese_utf8("㐀"));
    EXPECT_TRUE(tr::has_japanese_utf8("Chapter 1: 第一章"));
    EXPECT_TRUE(tr::has_japanese_utf8("「おはよう」"));

    EXPECT_FALSE(tr::has_japanese_utf8(""));
    EXPECT_FALSE(tr::has_japanese_utf8("Plain English dialogue."));
    EXPECT_FALSE(tr::has_japanese_utf8("café — résumé …"));
    EXPECT_FALSE(tr::has_japanese_utf8("？？？"));

    EXPECT_FALSE(tr::has_japanese_utf8(bytes_to_string(kTruncatedThreeByteHiragana)));
    EXPECT_FALSE(tr::has_japanese_utf8(bytes_to_string(kInvalidContinuationThreeByte)));
    EXPECT_FALSE(tr::has_japanese_utf8(bytes_to_string(kInvalidLeadByteBeforeKanji)));
}
