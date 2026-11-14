// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// System tests validating the staged install/game/translation_table.tsv
// artifact on disk and through translator_logic's UTF-16 table and runtime
// post-hook facade.

#include "translator_logic.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>

#ifndef GAME_TSV_PATH
#define GAME_TSV_PATH ""
#endif

namespace {

namespace fs = std::filesystem;

using translator_logic::RuntimeStats;
using translator_logic::ShouldBypassTranslation;
using translator_logic::TranslationRuntimeFacade;
using translator_logic::TranslationTable;
using translator_logic::Utf16ToUtf8;
using translator_logic::Utf8ToUtf16;

constexpr std::size_t kZeroCount = 0u;
constexpr std::size_t kSingleCharCount = 1u;
constexpr std::size_t kMinExpectedTableRows = 10u;
constexpr std::size_t kUtf8BomLength = 3u;
constexpr std::uint8_t kUtf8BomByte0 = 0xEFu;
constexpr std::uint8_t kUtf8BomByte1 = 0xBBu;
constexpr std::uint8_t kUtf8BomByte2 = 0xBFu;
constexpr std::size_t kExpectedMacroBypassCount = 2u;
constexpr std::size_t kExpectedSampleHitCount = 1u;

constexpr std::wstring_view kNamaeSurnameMacro = L"＊Ａ";
constexpr std::wstring_view kNamaeGivenMacro = L"＊Ｂ";

struct RawTsvScanSummary {
    std::size_t non_empty_rows = kZeroCount;
    std::size_t malformed_rows = kZeroCount;
};

std::string_view StripOptionalUtf8Bom(std::string_view bytes) noexcept {
    if (bytes.size() >= kUtf8BomLength &&
        static_cast<std::uint8_t>(bytes[0]) == kUtf8BomByte0 &&
        static_cast<std::uint8_t>(bytes[1]) == kUtf8BomByte1 &&
        static_cast<std::uint8_t>(bytes[2]) == kUtf8BomByte2) {
        bytes.remove_prefix(kUtf8BomLength);
    }
    return bytes;
}

RawTsvScanSummary ScanRawTsvRows(std::string_view raw_bytes) {
    RawTsvScanSummary summary{};
    std::string_view body = StripOptionalUtf8Bom(raw_bytes);
    std::size_t line_start = kZeroCount;
    const std::size_t total = body.size();
    std::wstring jp_wide;
    std::wstring en_wide;

    while (line_start < total) {
        const std::size_t nl = body.find('\n', line_start);
        const std::size_t line_end =
            (nl == std::string_view::npos) ? total : nl;

        std::string_view line = body.substr(line_start, line_end - line_start);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(kSingleCharCount);
        }
        line_start =
            (nl == std::string_view::npos) ? total : (nl + kSingleCharCount);
        if (line.empty()) {
            continue;
        }

        ++summary.non_empty_rows;

        const std::size_t tab = line.find('\t');
        if (tab == std::string_view::npos) {
            ++summary.malformed_rows;
            continue;
        }

        const std::string_view raw_jp = line.substr(kZeroCount, tab);
        const std::string_view raw_en = line.substr(tab + kSingleCharCount);
        if (raw_jp.empty() || raw_en.empty() ||
            raw_en.find('\t') != std::string_view::npos) {
            ++summary.malformed_rows;
            continue;
        }

        if (!Utf8ToUtf16(raw_jp, jp_wide) || !Utf8ToUtf16(raw_en, en_wide)) {
            ++summary.malformed_rows;
        }
    }

    return summary;
}

}  // namespace

class StagedTranslationTableTest : public ::testing::Test {
protected:
    fs::path tsv_path_;
    std::string raw_bytes_;
    TranslationTable table_;

    void SetUp() override {
        ASSERT_STRNE(GAME_TSV_PATH, "");

        std::wstring wide_path;
        ASSERT_TRUE(Utf8ToUtf16(GAME_TSV_PATH, wide_path))
            << "GAME_TSV_PATH is not valid UTF-8: " << GAME_TSV_PATH;
        ASSERT_FALSE(wide_path.empty());
        tsv_path_ = fs::path(wide_path);

        std::error_code ec;
        const bool exists = fs::exists(tsv_path_, ec);
        ASSERT_FALSE(static_cast<bool>(ec))
            << "Failed querying staged TSV path " << tsv_path_.u8string()
            << ": " << ec.message();
        if (!exists) {
            GTEST_SKIP() << "Staged translation_table.tsv not present at "
                         << tsv_path_.u8string();
        }

        ASSERT_TRUE(fs::is_regular_file(tsv_path_, ec))
            << "Staged TSV path is not a regular file: "
            << tsv_path_.u8string();
        ASSERT_FALSE(static_cast<bool>(ec));

        std::ifstream in(tsv_path_, std::ios::binary);
        ASSERT_TRUE(in.is_open())
            << "Failed opening staged translation_table.tsv at "
            << tsv_path_.u8string();
        raw_bytes_.assign(std::istreambuf_iterator<char>(in),
                          std::istreambuf_iterator<char>());
        ASSERT_FALSE(in.bad())
            << "I/O error while reading staged translation_table.tsv at "
            << tsv_path_.u8string();
        ASSERT_FALSE(raw_bytes_.empty())
            << "Staged translation_table.tsv is empty at "
            << tsv_path_.u8string();

        ASSERT_TRUE(table_.LoadFromFile(tsv_path_))
            << "TranslationTable failed to parse staged translation_table.tsv "
               "at "
            << tsv_path_.u8string();
    }
};

TEST_F(StagedTranslationTableTest, HasReasonableSize) {
    EXPECT_GT(raw_bytes_.size(), kZeroCount);
    EXPECT_FALSE(table_.Empty());
    EXPECT_GT(table_.Size(), kMinExpectedTableRows);

    TranslationTable utf8_loaded;
    ASSERT_TRUE(utf8_loaded.LoadFromUtf8Path(GAME_TSV_PATH));
    EXPECT_EQ(utf8_loaded.Size(), table_.Size());
}

TEST_F(StagedTranslationTableTest, EveryKeyAndValueIsNonEmptyAndNullFree) {
    EXPECT_EQ(raw_bytes_.find('\0'), std::string::npos);

    const RawTsvScanSummary raw_summary = ScanRawTsvRows(raw_bytes_);
    EXPECT_GT(raw_summary.non_empty_rows, kMinExpectedTableRows);
    EXPECT_EQ(raw_summary.malformed_rows, kZeroCount);
    EXPECT_EQ(table_.Size(), raw_summary.non_empty_rows);

    std::size_t visited_rows = kZeroCount;
    std::size_t invalid_rows = kZeroCount;
    std::size_t lookup_mismatches = kZeroCount;
    std::string utf8_jp;
    std::string utf8_en;

    table_.ForEach([this, &visited_rows, &invalid_rows, &lookup_mismatches,
                    &utf8_jp,
                    &utf8_en](std::wstring_view jp, std::wstring_view en) {
        ++visited_rows;
        if (jp.empty() || en.empty() ||
            jp.find(L'\0') != std::wstring_view::npos ||
            en.find(L'\0') != std::wstring_view::npos ||
            !Utf16ToUtf8(jp, utf8_jp) || !Utf16ToUtf8(en, utf8_en)) {
            ++invalid_rows;
            return;
        }

        const std::wstring* looked_up = table_.Find(jp);
        if (looked_up == nullptr || *looked_up != en) {
            ++lookup_mismatches;
        }
    });

    EXPECT_EQ(visited_rows, table_.Size());
    EXPECT_EQ(invalid_rows, kZeroCount);
    EXPECT_EQ(lookup_mismatches, kZeroCount);
}

TEST_F(StagedTranslationTableTest, ExcludesProtagonistNamaeMacroKeys) {
    EXPECT_FALSE(table_.Contains(L"＊Ａ"));
    EXPECT_FALSE(table_.Contains(L"＊Ｂ"));
    EXPECT_EQ(table_.Find(kNamaeSurnameMacro), nullptr);
    EXPECT_EQ(table_.Find(kNamaeGivenMacro), nullptr);

    std::size_t macro_key_hits = kZeroCount;
    std::size_t bypassed_key_hits = kZeroCount;
    std::wstring sample_jp;

    table_.ForEach([&macro_key_hits, &bypassed_key_hits, &sample_jp](
                       std::wstring_view jp, std::wstring_view /*en*/) {
        if (jp == kNamaeSurnameMacro || jp == kNamaeGivenMacro) {
            ++macro_key_hits;
        }
        if (ShouldBypassTranslation(jp)) {
            ++bypassed_key_hits;
            return;
        }
        if (sample_jp.empty()) {
            sample_jp.assign(jp);
        }
    });

    EXPECT_EQ(macro_key_hits, kZeroCount);
    EXPECT_EQ(bypassed_key_hits, kZeroCount);
    ASSERT_FALSE(sample_jp.empty());

    TranslationRuntimeFacade runtime;
    ASSERT_TRUE(runtime.LoadTsvFile(tsv_path_));
    EXPECT_EQ(runtime.EntryCount(), table_.Size());

    bool macro_callback_invoked = false;
    const auto fail_on_macro_mutation =
        [&macro_callback_invoked](std::wstring_view /*replacement*/) {
            macro_callback_invoked = true;
        };

    EXPECT_FALSE(runtime.ApplyPostHook(L"＊Ａ", fail_on_macro_mutation));
    EXPECT_FALSE(runtime.ApplyPostHook(L"＊Ｂ", fail_on_macro_mutation));
    EXPECT_FALSE(macro_callback_invoked);

    std::wstring translated_sample;
    EXPECT_TRUE(runtime.ApplyPostHook(
        sample_jp,
        [&translated_sample](std::wstring_view replacement) {
            translated_sample.assign(replacement);
        }));
    EXPECT_FALSE(translated_sample.empty());

    const RuntimeStats stats = runtime.Stats();
    EXPECT_EQ(stats.bypassed_calls, kExpectedMacroBypassCount);
    EXPECT_EQ(stats.translated_hits, kExpectedSampleHitCount);
    EXPECT_EQ(stats.lookup_misses, kZeroCount);
}
