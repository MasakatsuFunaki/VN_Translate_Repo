// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Clean-checkout component integration tests for the sakuranotoki proxy_dll
// file-backed translation table pipeline (`tr::load_tsv_file`, `tr::lookup`,
// and `tr::has_japanese_utf8`) over a Unicode temporary directory.
#include "translator_logic.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>

namespace {

namespace fs = std::filesystem;

constexpr std::wstring_view kUnicodeTempPrefix = L"桜ノ刻_proxy_ct_";
constexpr std::size_t kBomByteCount = 3U;
constexpr std::array<std::uint8_t, kBomByteCount> kUtf8BomBytes = {{
    0xEFU,
    0xBBU,
    0xBFU,
}};
constexpr std::size_t kExpectedLoadedRowCount = 6U;

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::wstring_view prefix) {
        static std::atomic<std::uint64_t> seq{0};
        const auto tick = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const std::wstring leaf = std::wstring(prefix) + std::to_wstring(tick) +
                                  L"_" + std::to_wstring(++seq);
        path_ = fs::temp_directory_path() / fs::path(leaf);
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    ~ScopedTempDir() noexcept {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

void WriteUtf8TsvWithBom(const fs::path& file_path, std::string_view utf8_body) {
    std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(static_cast<bool>(out));
    out.write(reinterpret_cast<const char*>(kUtf8BomBytes.data()),
              static_cast<std::streamsize>(kUtf8BomBytes.size()));
    out.write(utf8_body.data(), static_cast<std::streamsize>(utf8_body.size()));
    ASSERT_TRUE(static_cast<bool>(out));
}

}  // namespace

TEST(TranslationPipelineCt,
     FileLoadLookupAndJapaneseGuardRoundtripAcrossUnicodeDirectory) {
    const ScopedTempDir temp_dir(kUnicodeTempPrefix);
    const fs::path tsv_path = temp_dir.path() / fs::u8path("translations.tsv");

    constexpr std::string_view kMultiEntryTsv =
        "# Generated runtime translation table\r\n"
        "直哉\tNaoya\r\n"
        "心鈴\tMisuzu\r\n"
        "ルリヲ\tRuriwo\r\n"
        "「おはよう。\\nいい朝だな」\t\"Good morning.\\nIt is a fine morning.\"\r\n"
        "　桜の花びらが舞っていた。\t　Cherry petals were dancing in the air.\r\n"
        "章\\t一\\\\記録\tChapter\\tOne\\\\Record\r\n";

    WriteUtf8TsvWithBom(tsv_path, kMultiEntryTsv);

    std::unordered_map<std::string, std::string> table;
    const std::size_t loaded = tr::load_tsv_file(tsv_path.u8string(), table);
    ASSERT_EQ(loaded, kExpectedLoadedRowCount);
    EXPECT_EQ(table.size(), kExpectedLoadedRowCount);

    for (const auto& [jp, en] : table) {
        EXPECT_FALSE(jp.empty());
        EXPECT_FALSE(en.empty());
        EXPECT_EQ(jp.find('\0'), std::string::npos);
        EXPECT_EQ(en.find('\0'), std::string::npos);
        EXPECT_TRUE(tr::has_japanese_utf8(jp)) << jp;
        EXPECT_FALSE(tr::has_japanese_utf8(en)) << en;
    }

    std::string resolved;
    ASSERT_TRUE(tr::lookup(table, "直哉", resolved));
    EXPECT_EQ(resolved, "Naoya");

    ASSERT_TRUE(tr::lookup(table, "ルリヲ", resolved));
    EXPECT_EQ(resolved, "Ruriwo");

    const std::string multiline_jp = "「おはよう。\nいい朝だな」";
    ASSERT_TRUE(tr::has_japanese_utf8(multiline_jp));
    ASSERT_TRUE(tr::lookup(table, multiline_jp, resolved));
    EXPECT_EQ(resolved, "\"Good morning.\nIt is a fine morning.\"");
    EXPECT_FALSE(tr::has_japanese_utf8(resolved));

    const std::string indented_jp = "　桜の花びらが舞っていた。";
    ASSERT_TRUE(tr::has_japanese_utf8(indented_jp));
    ASSERT_TRUE(tr::lookup(table, indented_jp, resolved));
    EXPECT_EQ(resolved, "　Cherry petals were dancing in the air.");
    EXPECT_FALSE(tr::has_japanese_utf8(resolved));

    ASSERT_TRUE(tr::lookup(table, "章\t一\\記録", resolved));
    EXPECT_EQ(resolved, "Chapter\tOne\\Record");

    std::string sentinel = "unchanged";
    EXPECT_FALSE(tr::has_japanese_utf8("？？？"));
    EXPECT_FALSE(tr::lookup(table, "？？？", sentinel));
    EXPECT_EQ(sentinel, "unchanged");
}
