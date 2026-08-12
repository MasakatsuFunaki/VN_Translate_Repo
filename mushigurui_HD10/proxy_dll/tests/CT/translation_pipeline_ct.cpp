// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// CT — Translation pipeline component tests for mushigurui_HD10.
//
// End-to-end: read the deployed translations.tsv from disk, parse it,
// validate post-load invariants the rest of the DLL depends on. SKIPs
// cleanly if the TSV doesn't exist (machine-dependent file).

#include "translator_logic.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#  include <wchar.h>
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

using translator_logic::TranslationMap;
using translator_logic::ParseTsvBuffer;

#ifndef GAME_TSV_PATH
#  define GAME_TSV_PATH ""
#endif

namespace {

bool LoadRealTsv(TranslationMap& out) {
#ifdef _WIN32
    const char* narrow = GAME_TSV_PATH;
    if (!narrow || !*narrow) return false;
    int wn = MultiByteToWideChar(CP_UTF8, 0, narrow, -1, nullptr, 0);
    if (wn <= 0) return false;
    std::wstring wpath(static_cast<size_t>(wn), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, narrow, -1, &wpath[0], wn);
    if (!wpath.empty() && wpath.back() == L'\0') wpath.pop_back();

    FILE* f = _wfopen(wpath.c_str(), L"rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) { std::fclose(f); return false; }
    std::vector<char> buf(static_cast<size_t>(sz));
    size_t n = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (n == 0) return false;
    return ParseTsvBuffer(buf.data(), n, out) > 0;
#else
    return false;
#endif
}

}  // namespace

class TranslationPipelineTest : public ::testing::Test {
protected:
    TranslationMap map;
    void SetUp() override {
        if (!LoadRealTsv(map)) {
            GTEST_SKIP()
                << "translations.tsv not found at " << GAME_TSV_PATH
                << " (skip is OK on machines without the game data).";
        }
    }
};

TEST_F(TranslationPipelineTest, HasReasonableSize) {
    EXPECT_GT(map.size(), 10u);
}

TEST_F(TranslationPipelineTest, EveryValueIsNullFree) {
    size_t bad = 0;
    for (const auto& kv : map) {
        if (kv.second.find('\0') != std::string::npos) ++bad;
    }
    EXPECT_EQ(bad, 0u);
}

TEST_F(TranslationPipelineTest, NoEmptyKeysOrValues) {
    for (const auto& kv : map) {
        EXPECT_FALSE(kv.first.empty());
        EXPECT_FALSE(kv.second.empty());
    }
}
