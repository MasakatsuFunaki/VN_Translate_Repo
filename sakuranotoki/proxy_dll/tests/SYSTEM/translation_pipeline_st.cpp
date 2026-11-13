// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// System-tier integration tests for the sakuranotoki proxy_dll translation
// lookup table over a locally generated `translations.tsv`, auto-skipping when
// local TSV artifacts have not been built yet.
#include "translator_logic.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <unordered_map>

#ifndef VN_PROJECT_ROOT_DIR
#define VN_PROJECT_ROOT_DIR ""
#endif

namespace {

namespace fs = std::filesystem;

constexpr std::size_t kMinExpectedTableSize = 10;

fs::path resolve_deployed_tsv_path() {
    const std::string root_str = VN_PROJECT_ROOT_DIR;
    if (root_str.empty()) {
        return {};
    }
    const fs::path root = fs::u8path(root_str);
    const fs::path root_tsv = root / "translations.tsv";
    if (fs::exists(root_tsv)) {
        return root_tsv;
    }
    const fs::path staged_tsv = root / "build" / "install" / "game" / "translations.tsv";
    if (fs::exists(staged_tsv)) {
        return staged_tsv;
    }
    return {};
}

class TranslationPipelineSt : public ::testing::Test {
protected:
    std::unordered_map<std::string, std::string> table_;
    std::size_t inserted_count_ = 0;

    void SetUp() override {
        const fs::path tsv_path = resolve_deployed_tsv_path();
        if (tsv_path.empty()) {
            GTEST_SKIP() << "translations.tsv not present under "
                         << VN_PROJECT_ROOT_DIR;
        }
        inserted_count_ = tr::load_tsv_file(tsv_path.u8string(), table_);
        ASSERT_NE(inserted_count_, SIZE_MAX)
            << "failed to load " << tsv_path.u8string();
    }
};

}  // namespace

TEST_F(TranslationPipelineSt, LoadsNonTrivialTableWithNonEmptyColumns) {
    EXPECT_GE(inserted_count_, kMinExpectedTableSize);
    EXPECT_EQ(table_.size(), inserted_count_);

    std::size_t embedded_nulls = 0;
    for (const auto& [jp, en] : table_) {
        EXPECT_FALSE(jp.empty());
        EXPECT_FALSE(en.empty());
        if (jp.find('\0') != std::string::npos || en.find('\0') != std::string::npos) {
            ++embedded_nulls;
        }
    }
    EXPECT_EQ(embedded_nulls, 0U);
}

TEST_F(TranslationPipelineSt, EndToEndLookupAndJapaneseClassificationHold) {
    ASSERT_TRUE(tr::has_japanese_utf8("直哉"));

    std::string resolved;
    ASSERT_TRUE(tr::lookup(table_, "直哉", resolved));
    EXPECT_EQ(resolved, "Naoya");
    EXPECT_FALSE(tr::has_japanese_utf8(resolved));

    std::size_t japanese_key_count = 0;
    for (const auto& [jp, en] : table_) {
        std::string hit;
        ASSERT_TRUE(tr::lookup(table_, jp, hit));
        EXPECT_EQ(hit, en);
        if (tr::has_japanese_utf8(jp)) {
            ++japanese_key_count;
        }
    }
    EXPECT_GT(japanese_key_count, 0U);

    std::string sentinel = "unchanged";
    EXPECT_FALSE(tr::lookup(table_, "存在しないテスト行", sentinel));
    EXPECT_EQ(sentinel, "unchanged");
}
