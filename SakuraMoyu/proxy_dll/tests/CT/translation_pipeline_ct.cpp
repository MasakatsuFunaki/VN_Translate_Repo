// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Component tests for the FAVORITE translation pipeline.
//
// Reads the staged translation_table.tsv from disk, parses it, and validates
// post-load invariants relied upon by the proxy DLL.
#include "translator_logic.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using translator_logic::ParseTsvBuffer;
using translator_logic::TranslationMap;

#ifndef GAME_TSV_PATH
#define GAME_TSV_PATH ""
#endif

namespace {

constexpr std::size_t kMinExpectedTranslations = 10u;
constexpr std::size_t kExpectedCorruptEntryCount = 0u;
constexpr std::size_t kSingleByteElementSize = 1u;
constexpr int kMinimumPositiveParsedCount = 0;
constexpr long kFileStartOffset = 0L;
constexpr long kEmptyOrFailedTellSize = 0L;

bool LoadRealTsv(TranslationMap& out) {
#ifdef _WIN32
    const char* const narrow = GAME_TSV_PATH;
    if (narrow == nullptr || *narrow == '\0') {
        return false;
    }
    const int narrow_len = static_cast<int>(std::strlen(narrow));
    const int wide_len = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, narrow, narrow_len, nullptr, 0);
    if (wide_len <= 0) {
        return false;
    }
    std::wstring wpath(static_cast<std::size_t>(wide_len), L'\0');
    const int converted = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, narrow, narrow_len, wpath.data(), wide_len);
    if (converted != wide_len) {
        return false;
    }

    using ScopedFile = std::unique_ptr<std::FILE, decltype(&std::fclose)>;
    ScopedFile file(_wfopen(wpath.c_str(), L"rb"), &std::fclose);
    if (!file) {
        return false;
    }
    if (std::fseek(file.get(), kFileStartOffset, SEEK_END) != 0) {
        return false;
    }
    const long sz = std::ftell(file.get());
    if (sz <= kEmptyOrFailedTellSize) {
        return false;
    }
    if (std::fseek(file.get(), kFileStartOffset, SEEK_SET) != 0) {
        return false;
    }
    std::vector<char> buf(static_cast<std::size_t>(sz));
    const std::size_t bytes_read =
        std::fread(buf.data(), kSingleByteElementSize, buf.size(), file.get());
    if (bytes_read == 0u) {
        return false;
    }
    return ParseTsvBuffer(buf.data(), bytes_read, out) > kMinimumPositiveParsedCount;
#else
    (void)out;
    return false;
#endif
}

}  // namespace

class TranslationPipelineTest : public ::testing::Test {
protected:
    TranslationMap map;

    void SetUp() override {
        ASSERT_TRUE(LoadRealTsv(map))
            << "Missing or empty deployed TSV: " << GAME_TSV_PATH;
    }
};

TEST_F(TranslationPipelineTest, HasReasonableSize) {
    EXPECT_GT(map.size(), kMinExpectedTranslations);
}

TEST_F(TranslationPipelineTest, EveryValueIsNullFree) {
    std::size_t bad = 0u;
    for (const auto& kv : map) {
        if (kv.second.find('\0') != std::string::npos) {
            ++bad;
        }
    }
    EXPECT_EQ(bad, kExpectedCorruptEntryCount);
}

TEST_F(TranslationPipelineTest, NoEmptyKeysOrValues) {
    for (const auto& kv : map) {
        EXPECT_FALSE(kv.first.empty());
        EXPECT_FALSE(kv.second.empty());
    }
}
