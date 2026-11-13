// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Component tests for the Mushigurui proxy_dll runtime artifacts: validates the
// generated translation_table.tsv (row counts, null/empty invariants, 100%
// verbatim + trailing-CRLF-stripped lookup parity across all 13,105 rows,
// runtime UI fallback seeding, and CP932 `#` sanitization) and verifies the
// committed menu_patch.bin ("MSGMENU1" v1) wire layout and exact EOF.

#include "menu_autopatch.h"
#include "translator_logic.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#ifndef GAME_TSV_PATH
#define GAME_TSV_PATH ""
#endif

#ifndef FALLBACK_TSV_PATH
#define FALLBACK_TSV_PATH ""
#endif

#ifndef MENU_PATCH_BIN_PATH
#define MENU_PATCH_BIN_PATH ""
#endif

namespace fs = std::filesystem;
namespace tl = translator_logic;

namespace {

constexpr int kExpectedMushiguruiTsvRowCount = 13105;
constexpr std::size_t kExpectedMushiguruiUniqueKeys = 12384;
constexpr int kExpectedSanitizedYellowChoiceRows = 2;
constexpr std::size_t kCp932ColorTagBytes = 3;

constexpr std::size_t kPatchMagicSize = 8u;
constexpr char kPatchMagic[kPatchMagicSize] = {'M', 'S', 'G', 'M', 'E', 'N', 'U', '1'};
constexpr std::size_t kPatchHeaderSize = 32u;
constexpr std::uint32_t kExpectedPatchVersion = 1u;
constexpr std::uint32_t kExpectedStandaloneGroups = 3u;
constexpr std::uint32_t kExpectedArchiveSections = 3u;
constexpr std::size_t kExpectedPatchedEntryTotal = 74u;

constexpr std::size_t kHeaderVersionOffset = 0x08u;
constexpr std::size_t kHeaderStandaloneCountOffset = 0x0Cu;
constexpr std::size_t kHeaderArchiveCountOffset = 0x10u;
constexpr std::size_t kHeaderFlagsOffset = 0x14u;
constexpr std::size_t kHeaderReservedOffset = 0x18u;

constexpr std::size_t kU16Bytes = 2u;
constexpr std::size_t kStandaloneVariantHeaderBytes = 16u;
constexpr std::size_t kVariantNewSizeOffset = 8u;
constexpr std::size_t kArchiveFixedHeaderBytes = 36u;
constexpr std::size_t kArchiveOrigGtbSizeOffset = 16u;
constexpr std::size_t kArchiveNewGtbSizeOffset = 24u;
constexpr std::size_t kArchiveEntryCountOffset = 32u;
constexpr std::size_t kArchiveEntryHeaderBytes = 28u;
constexpr std::size_t kEntryNewSizeOffset = 20u;

constexpr unsigned int kShiftByte1 = 8u;
constexpr unsigned int kShiftByte2 = 16u;
constexpr unsigned int kShiftByte3 = 24u;
constexpr unsigned int kShiftByte4 = 32u;
constexpr unsigned int kShiftByte5 = 40u;
constexpr unsigned int kShiftByte6 = 48u;
constexpr unsigned int kShiftByte7 = 56u;

std::uint16_t ReadU16Le(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(p[0]) |
        (static_cast<std::uint16_t>(p[1]) << kShiftByte1));
}

std::uint32_t ReadU32Le(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << kShiftByte1) |
           (static_cast<std::uint32_t>(p[2]) << kShiftByte2) |
           (static_cast<std::uint32_t>(p[3]) << kShiftByte3);
}

std::uint64_t ReadU64Le(const std::uint8_t* p) noexcept {
    return static_cast<std::uint64_t>(p[0]) |
           (static_cast<std::uint64_t>(p[1]) << kShiftByte1) |
           (static_cast<std::uint64_t>(p[2]) << kShiftByte2) |
           (static_cast<std::uint64_t>(p[3]) << kShiftByte3) |
           (static_cast<std::uint64_t>(p[4]) << kShiftByte4) |
           (static_cast<std::uint64_t>(p[5]) << kShiftByte5) |
           (static_cast<std::uint64_t>(p[6]) << kShiftByte6) |
           (static_cast<std::uint64_t>(p[7]) << kShiftByte7);
}

bool ReadFileBytes(const char* utf8_path, std::vector<char>& out_bytes) {
    out_bytes.clear();
    if (utf8_path == nullptr || utf8_path[0] == '\0') {
        return false;
    }
    std::ifstream file(fs::u8path(utf8_path), std::ios::binary | std::ios::ate);
    if (!file) {
        return false;
    }
    const std::streamsize sz = file.tellg();
    if (sz <= 0) {
        return false;
    }
    file.seekg(0, std::ios::beg);
    out_bytes.resize(static_cast<std::size_t>(sz));
    return static_cast<bool>(file.read(out_bytes.data(), sz));
}

bool LoadRealTsvBytes(std::vector<char>& out_bytes) {
    if (ReadFileBytes(GAME_TSV_PATH, out_bytes)) {
        return true;
    }
    return ReadFileBytes(FALLBACK_TSV_PATH, out_bytes);
}

std::vector<std::pair<std::string, std::string>> ParseOrderedTsvPairs(
    const char* data,
    std::size_t len) {
    std::vector<std::pair<std::string, std::string>> rows;
    const std::size_t bom = tl::Utf8BomLen(data, len);
    std::size_t i = bom;
    while (i < len) {
        const std::size_t start = i;
        while (i < len && data[i] != '\n' && data[i] != '\r') {
            ++i;
        }
        const std::size_t end = i;
        while (i < len && (data[i] == '\r' || data[i] == '\n')) {
            ++i;
        }
        std::size_t tab = start;
        while (tab < end && data[tab] != '\t') {
            ++tab;
        }
        if (tab == start || tab >= end) {
            continue;
        }
        std::string jp(data + start, tab - start);
        std::string en(data + tab + 1, end - (tab + 1));
        tl::UnescapeInPlace(jp);
        tl::UnescapeInPlace(en);
        if (!jp.empty() && !en.empty()) {
            rows.emplace_back(std::move(jp), std::move(en));
        }
    }
    return rows;
}

bool ContainsUnvalidatedAsciiHash(const std::string& s) noexcept {
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '#') {
            continue;
        }
        if (i + kCp932ColorTagBytes > s.size() ||
            !tl::HasCp932ColorPrefix(s.c_str() + i)) {
            return true;
        }
        i += kCp932ColorTagBytes - 1;
    }
    return false;
}

bool ContainsLoneLfWithoutPrecedingCr(const std::string& s) noexcept {
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n' && (i == 0 || s[i - 1] != '\r')) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST(TranslationPipelineComponentTest, FullTsvResolves100PercentVerbatimAndCrLfStripped) {
    std::vector<char> tsv_bytes;
    ASSERT_TRUE(LoadRealTsvBytes(tsv_bytes))
        << "Expected generated translation_table.tsv at " << GAME_TSV_PATH
        << " or " << FALLBACK_TSV_PATH;

    tl::TranslationMap map;
    const std::size_t bom = tl::Utf8BomLen(tsv_bytes.data(), tsv_bytes.size());
    ASSERT_EQ(tl::ParseTsvBuffer(tsv_bytes.data() + bom, tsv_bytes.size() - bom, map),
              kExpectedMushiguruiTsvRowCount);
    EXPECT_EQ(map.size(), kExpectedMushiguruiUniqueKeys);

    std::size_t null_or_empty_entries = 0;
    for (const auto& [key, val] : map) {
        if (key.empty() || val.empty() ||
            key.find('\0') != std::string::npos ||
            val.find('\0') != std::string::npos) {
            ++null_or_empty_entries;
        }
    }
    EXPECT_EQ(null_or_empty_entries, 0u);

    tl::TranslationTable table;
    const int loaded_rows = table.LoadFromTsvBuffer(tsv_bytes.data(), tsv_bytes.size());
    EXPECT_EQ(loaded_rows, kExpectedMushiguruiTsvRowCount);
    EXPECT_EQ(table.ExactSize(), kExpectedMushiguruiUniqueKeys);
    EXPECT_EQ(table.StrippedSize(), kExpectedMushiguruiUniqueKeys);

    table.SeedRuntimeUiFallbacks();
    EXPECT_EQ(table.ExactSize(),
              kExpectedMushiguruiUniqueKeys + tl::kRuntimeUiFallbackCount);

    const auto rows = ParseOrderedTsvPairs(tsv_bytes.data(), tsv_bytes.size());
    ASSERT_EQ(static_cast<int>(rows.size()), kExpectedMushiguruiTsvRowCount);

    int exact_hits = 0;
    int stripped_hits = 0;
    int sanitized_yellow_choices = 0;
    std::size_t lone_lf_jp_keys = 0;
    std::size_t unvalidated_hash_outputs = 0;

    for (const auto& [jp, raw_en] : rows) {
        if (ContainsLoneLfWithoutPrecedingCr(jp)) {
            ++lone_lf_jp_keys;
        }

        std::string exact_out;
        if (table.Lookup(jp.c_str(), false, exact_out) && !exact_out.empty()) {
            ++exact_hits;
        }
        if (ContainsUnvalidatedAsciiHash(exact_out)) {
            ++unvalidated_hash_outputs;
        }

        const std::string ce_jp = tl::StripTrailingCrLf(jp);
        std::string stripped_out;
        if (table.Lookup(ce_jp.c_str(), false, stripped_out) && !stripped_out.empty()) {
            ++stripped_hits;
        }
        if (ContainsUnvalidatedAsciiHash(stripped_out)) {
            ++unvalidated_hash_outputs;
        }

        if (raw_en.rfind("# ", 0) == 0) {
            ++sanitized_yellow_choices;
            EXPECT_TRUE(tl::HasCp932ColorPrefix(exact_out.c_str()));
            EXPECT_EQ(exact_out.find("# "), std::string::npos);
            EXPECT_EQ(stripped_out, exact_out);
        }
    }

    EXPECT_EQ(lone_lf_jp_keys, 0u);
    EXPECT_EQ(exact_hits, kExpectedMushiguruiTsvRowCount);
    EXPECT_EQ(stripped_hits, kExpectedMushiguruiTsvRowCount);
    EXPECT_EQ(sanitized_yellow_choices, kExpectedSanitizedYellowChoiceRows);
    EXPECT_EQ(unvalidated_hash_outputs, 0u);

    const std::wstring kProbeBarsHd10 =
        L"完全に意識が途切れる寸前、レンが聞いた声は――――\r\n";
    const std::wstring kProbeMidlineHd10 =
        L"彼らに気付かれないようにして洞窟内に侵入するのは、さすがに不可能\r\nだった。\r\n";

    for (const std::wstring& wide_hd10 : {kProbeBarsHd10, kProbeMidlineHd10}) {
        std::string cp932_hd10;
        ASSERT_TRUE(tl::EncodeWideToCp932(wide_hd10, cp932_hd10));
        const std::string cp932_ce = tl::StripTrailingCrLf(cp932_hd10);

        std::string out_hd10;
        ASSERT_TRUE(table.Lookup(cp932_hd10.c_str(), false, out_hd10));
        EXPECT_FALSE(out_hd10.empty());

        std::string out_ce;
        ASSERT_TRUE(table.Lookup(cp932_ce.c_str(), false, out_ce));
        EXPECT_EQ(out_ce, out_hd10);
    }
}

TEST(TranslationPipelineComponentTest, CommittedMenuPatchBinMatchesMsgMenu1ContractAndExactEof) {
    std::vector<char> raw_chars;
    ASSERT_TRUE(ReadFileBytes(MENU_PATCH_BIN_PATH, raw_chars))
        << "Expected committed menu_patch.bin at " << MENU_PATCH_BIN_PATH;
    ASSERT_GE(raw_chars.size(), kPatchHeaderSize);

    const auto* data = reinterpret_cast<const std::uint8_t*>(raw_chars.data());
    const std::size_t total_size = raw_chars.size();
    EXPECT_EQ(std::memcmp(data, kPatchMagic, kPatchMagicSize), 0);
    EXPECT_EQ(ReadU32Le(data + kHeaderVersionOffset), kExpectedPatchVersion);
    const std::uint32_t standalone_count = ReadU32Le(data + kHeaderStandaloneCountOffset);
    const std::uint32_t archive_count = ReadU32Le(data + kHeaderArchiveCountOffset);
    EXPECT_EQ(standalone_count, kExpectedStandaloneGroups);
    EXPECT_EQ(archive_count, kExpectedArchiveSections);
    EXPECT_EQ(ReadU32Le(data + kHeaderFlagsOffset), 0u);
    EXPECT_EQ(ReadU64Le(data + kHeaderReservedOffset), 0ull);
    EXPECT_NE(menu_autopatch::ComputeMurmurHash2(data, kPatchHeaderSize), 0u);

    std::size_t pos = kPatchHeaderSize;
    for (std::uint32_t g = 0; g < standalone_count; ++g) {
        ASSERT_LE(kU16Bytes, total_size - pos);
        const std::uint16_t path_len = ReadU16Le(data + pos);
        pos += kU16Bytes;
        ASSERT_GT(path_len, 0u);
        ASSERT_LE(static_cast<std::size_t>(path_len) + kU16Bytes, total_size - pos);
        pos += path_len;
        const std::uint16_t variant_count = ReadU16Le(data + pos);
        pos += kU16Bytes;
        ASSERT_GT(variant_count, 0u);
        for (std::uint16_t v = 0; v < variant_count; ++v) {
            ASSERT_LE(kStandaloneVariantHeaderBytes, total_size - pos);
            const std::uint32_t new_sz = ReadU32Le(data + pos + kVariantNewSizeOffset);
            pos += kStandaloneVariantHeaderBytes;
            ASSERT_LE(new_sz, total_size - pos);
            pos += new_sz;
        }
    }

    std::size_t patched_entries = 0;
    for (std::uint32_t a = 0; a < archive_count; ++a) {
        ASSERT_LE(kU16Bytes, total_size - pos);
        const std::uint16_t stem_len = ReadU16Le(data + pos);
        pos += kU16Bytes;
        ASSERT_GT(stem_len, 0u);
        ASSERT_LE(static_cast<std::size_t>(stem_len) + kArchiveFixedHeaderBytes,
                  total_size - pos);
        pos += stem_len;

        const std::uint32_t orig_gtb_sz = ReadU32Le(data + pos + kArchiveOrigGtbSizeOffset);
        const std::uint32_t new_gtb_sz = ReadU32Le(data + pos + kArchiveNewGtbSizeOffset);
        const std::uint32_t entry_count = ReadU32Le(data + pos + kArchiveEntryCountOffset);
        EXPECT_EQ(new_gtb_sz, orig_gtb_sz);
        pos += kArchiveFixedHeaderBytes;

        ASSERT_LE(new_gtb_sz, total_size - pos);
        pos += new_gtb_sz;

        for (std::uint32_t e = 0; e < entry_count; ++e) {
            ASSERT_LE(kArchiveEntryHeaderBytes, total_size - pos);
            const std::uint32_t entry_new_sz = ReadU32Le(data + pos + kEntryNewSizeOffset);
            pos += kArchiveEntryHeaderBytes;
            ASSERT_LE(entry_new_sz, total_size - pos);
            pos += entry_new_sz;
        }
        patched_entries += entry_count;
    }

    EXPECT_EQ(patched_entries, kExpectedPatchedEntryTotal);
    EXPECT_EQ(pos, total_size);
}
