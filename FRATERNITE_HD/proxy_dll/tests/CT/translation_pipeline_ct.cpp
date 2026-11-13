// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Translation pipeline component tests for FRATERNITE_HD.
//
// Reads the staged translation_table.tsv from disk, parses it, and validates
// post-load invariants required by the runtime proxy DLL.
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

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using translator_logic::ParseTsvBuffer;
using translator_logic::TranslationMap;

#ifndef GAME_TSV_PATH
#define GAME_TSV_PATH ""
#endif

namespace {

constexpr std::size_t kMinExpectedTsvEntries = 10u;

bool LoadRealTsv(TranslationMap& out) {
#ifdef _WIN32
    const char* narrow = GAME_TSV_PATH;
    if (narrow == nullptr || *narrow == '\0') {
        return false;
    }
    const int wn = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, narrow, -1, nullptr, 0);
    if (wn <= 0) {
        return false;
    }
    std::wstring wpath(static_cast<std::size_t>(wn), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, narrow, -1, wpath.data(), wn) != wn) {
        return false;
    }
    if (!wpath.empty() && wpath.back() == L'\0') {
        wpath.pop_back();
    }

    std::unique_ptr<FILE, decltype(&std::fclose)> file(
        _wfopen(wpath.c_str(), L"rb"), &std::fclose);
    if (!file) {
        return false;
    }
    if (std::fseek(file.get(), 0, SEEK_END) != 0) {
        return false;
    }
    const long sz = std::ftell(file.get());
    if (sz <= 0) {
        return false;
    }
    if (std::fseek(file.get(), 0, SEEK_SET) != 0) {
        return false;
    }
    std::vector<char> buf(static_cast<std::size_t>(sz));
    const std::size_t n = std::fread(buf.data(), 1, buf.size(), file.get());
    if (n == 0) {
        return false;
    }
    return ParseTsvBuffer(buf.data(), n, out) > 0;
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
            << "translation_table.tsv not found or empty at " << GAME_TSV_PATH;
    }
};

TEST_F(TranslationPipelineTest, HasReasonableSize) {
    EXPECT_GT(map.size(), kMinExpectedTsvEntries);
}

TEST_F(TranslationPipelineTest, EveryValueIsNullFree) {
    std::size_t bad = 0;
    for (const auto& kv : map) {
        if (kv.second.find('\0') != std::string::npos) {
            ++bad;
        }
    }
    EXPECT_EQ(bad, 0u);
}

TEST_F(TranslationPipelineTest, NoEmptyKeysOrValues) {
    for (const auto& kv : map) {
        EXPECT_FALSE(kv.first.empty());
        EXPECT_FALSE(kv.second.empty());
    }
}
