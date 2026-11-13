// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include "runtime_hooks.h"
#include "translator_logic.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace oretsuba::proxy {
namespace {

namespace fs = std::filesystem;

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view prefix) {
        static std::atomic<unsigned int> seq{0U};
        const unsigned int id = seq.fetch_add(1U, std::memory_order_relaxed);
        const std::string dir_name =
            std::string(prefix) + "_日本語_" + std::to_string(id);
        dir_ = fs::temp_directory_path() / fs::u8path(dir_name);
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_, ec);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const fs::path& path() const noexcept { return dir_; }

private:
    fs::path dir_;
};

TEST(TranslationPipelineComponentTest, ScratchRingPreservesConcurrentBacklogAndActiveMessages) {
    TranslationCatalog catalog;
    std::string tsv_rows;
    for (int i = 0; i < 150; ++i) {
        tsv_rows += "テスト行" + std::to_string(i) + "\tTranslated line number " +
                    std::to_string(i) + " in Gretagard.\n";
    }
    ASSERT_EQ(catalog.LoadFromString(tsv_rows), 150U);

    RuntimeHookManager hook_mgr(&catalog);

    std::vector<char*> recent_ptrs;
    for (int i = 0; i < 64; ++i) {
        const std::string raw = TranslationCatalog::Utf8ToCp932(
            "#タカシ\\00_00_" + std::to_string(i) + "$&$テスト行" + std::to_string(i) + "$");
        char* buf = hook_mgr.FormatIntoNextScratchBuffer(raw);
        ASSERT_NE(buf, nullptr);

        char* first_dollar = std::strchr(buf, '$');
        ASSERT_NE(first_dollar, nullptr);
        *first_dollar = '\0';
        recent_ptrs.push_back(buf);
    }

    for (int i = 0; i < 64; ++i) {
        const std::string expected_header = "#Takashi\\00_00_" + std::to_string(i);
        EXPECT_EQ(std::string(recent_ptrs[static_cast<std::size_t>(i)]), expected_header);
        const char* body_ptr =
            recent_ptrs[static_cast<std::size_t>(i)] + expected_header.size() + 3U;
        const std::string expected_body =
            "Translated line number " + std::to_string(i) + " in Gretagard.$";
        EXPECT_EQ(std::string(body_ptr), expected_body);
    }
}

TEST(TranslationPipelineComponentTest, LoadsUtf8AndWideFilePathsAndFormatsMessages) {
    const ScopedTempDir temp_dir("oretsuba_ct_tsv");
    const fs::path tsv_path = temp_dir.path() / fs::u8path("translation_table_日本語.tsv");
    {
        std::ofstream ofs(tsv_path, std::ios::binary);
        ASSERT_TRUE(ofs.is_open());
        ofs << "\xEF\xBB\xBF"
            << "# Comment header\r\n"
            << "タカシ\tTakashi\r\n"
            << "「ここはグレタガルドなのか？」\t\"Is this really Gretagard?\"\r\n"
            << "声をかける\tCall out to her\r\n";
    }

    TranslationCatalog narrow_catalog;
    EXPECT_EQ(narrow_catalog.LoadFromFile(static_cast<const char*>(nullptr)), 0U);
    EXPECT_EQ(narrow_catalog.LoadFromFile(""), 0U);
    EXPECT_EQ(narrow_catalog.LoadFromFile("\xFF\xFE"), 0U);
    ASSERT_EQ(narrow_catalog.LoadFromFile(tsv_path.u8string().c_str()), 3U);

    TranslationCatalog wide_catalog;
    EXPECT_EQ(wide_catalog.LoadFromFile(static_cast<const wchar_t*>(nullptr)), 0U);
    EXPECT_EQ(wide_catalog.LoadFromFile(L""), 0U);
    ASSERT_EQ(wide_catalog.LoadFromFile(tsv_path.wstring().c_str()), 3U);

    const std::string raw = TranslationCatalog::Utf8ToCp932(
        "#タカシ\\01_01_001$&$「ここはグレタガルドなのか？」$");
    EXPECT_EQ(narrow_catalog.TranslateMessageRecord(raw),
              "#Takashi\\01_01_001$&$\"Is this really Gretagard?\"$");
    EXPECT_EQ(wide_catalog.TranslateMessageRecord(raw),
              "#Takashi\\01_01_001$&$\"Is this really Gretagard?\"$");
}

}  // namespace
}  // namespace oretsuba::proxy
