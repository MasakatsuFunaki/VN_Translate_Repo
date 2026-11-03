// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include "runtime_hooks.h"
#include "translator_logic.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace oretsuba::proxy {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kMinExpectedProductionTsvEntries = 400;

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

        // Simulate 0x00455b49 mutating the first '$' to '\0'
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

TEST(TranslationPipelineComponentTest, LoadsProductionTsvWhenAvailableAndTranslatesCp932Records) {
    const fs::path tsv_path =
        fs::u8path(ORETSUBA_ROOT_DIR) / "build" / "install" / "game" / "translation_table.tsv";
    if (!fs::is_regular_file(tsv_path)) {
        GTEST_SKIP() << "Production translation_table.tsv not yet staged at "
                     << tsv_path.u8string();
    }

    TranslationCatalog catalog;
    const std::size_t count = catalog.LoadFromFile(tsv_path.wstring().c_str());
    EXPECT_GT(count, kMinExpectedProductionTsvEntries);

    std::ifstream ifs(tsv_path, std::ios::binary);
    ASSERT_TRUE(ifs.is_open());

    std::string line;
    std::size_t verified = 0;
    while (std::getline(ifs, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const std::size_t tab = line.find('\t');
        if (tab == std::string::npos) {
            continue;
        }
        const std::string jp_utf8 = line.substr(0, tab);
        const std::string jp_cp932 = TranslationCatalog::Utf8ToCp932(jp_utf8);
        ASSERT_FALSE(jp_cp932.empty()) << "Failed CP932 conversion for: " << jp_utf8;

        const auto hit = catalog.LookupTranslation(jp_cp932);
        ASSERT_TRUE(hit.has_value()) << "Failed CP932 lookup for: " << jp_utf8;
        EXPECT_FALSE(hit->empty());
        EXPECT_EQ(hit->find('\t'), std::string::npos) << "Corrupted tab in: " << *hit;
        EXPECT_EQ(hit->find("\\ow."), std::string::npos) << "Corrupted \\now in: " << *hit;

        const std::string msg_record =
            TranslationCatalog::Utf8ToCp932("#タカシ\\01_01_001$&$") + jp_cp932 + "$";
        const std::string formatted = catalog.TranslateMessageRecord(msg_record);
        EXPECT_EQ(formatted.rfind("#Takashi\\01_01_001$&$", 0), 0U);
        EXPECT_EQ(formatted.back(), '$');
        EXPECT_EQ(formatted.find('\t'), std::string::npos)
            << "Corrupted tab in formatted: " << formatted;
        EXPECT_EQ(formatted.find("\\ow."), std::string::npos)
            << "Corrupted \\now in formatted: " << formatted;
        ++verified;
    }
    EXPECT_EQ(verified, count);
}

}  // namespace
}  // namespace oretsuba::proxy
