// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// CT — End-to-end: load the deployed translations.tsv, validate post-load
// invariants.  Skips if the TSV doesn't exist.

#include "translator_logic.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <iostream>
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
using translator_logic::HasJPLeadByte;

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

TEST_F(TranslationPipelineTest, EveryKeyHasCp932LeadByte) {
    size_t bad = 0;
    std::string first_bad;
    for (const auto& kv : map) {
        if (!HasJPLeadByte(kv.first.data(), kv.first.size())) {
            ++bad;
            if (first_bad.empty()) first_bad = kv.first;
        }
    }
    EXPECT_EQ(bad, 0u)
        << bad << " keys lack a CP932 lead byte; first such: '"
        << first_bad << "'";
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

TEST_F(TranslationPipelineTest, EveryValueWrapsInsideTheMessageBox) {
    size_t widest = 0;
    std::string worst;
    for (const auto& kv : map) {
        const std::string text =
            translator_logic::WrapForMessageBox(kv.second).text;
        size_t pos = 0;
        for (;;) {
            size_t nl = text.find('\n', pos);
            size_t width = (nl == std::string::npos ? text.size() : nl) - pos;
            if (width > widest) { widest = width; worst = kv.second; }
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
    }
    EXPECT_LE(widest, translator_logic::BOX_COLUMNS)
        << "widest drawn line is " << widest << " columns; from: " << worst;
    std::cout << "[ NOTE     ] widest drawn line over the whole table: "
              << widest << " of " << translator_logic::BOX_COLUMNS
              << " columns\n";
}

TEST_F(TranslationPipelineTest, NoValueOutgrowsTheVisibleLines) {
    size_t truncated = 0;
    size_t longest = 0;
    std::string worst;
    for (const auto& kv : map) {
        translator_logic::WrappedText w =
            translator_logic::WrapForMessageBox(kv.second);
        EXPECT_LE(w.lines, translator_logic::MAX_LINES);
        if (w.truncated) { ++truncated; if (worst.empty()) worst = kv.second; }
        if (kv.second.size() > longest) longest = kv.second.size();
    }
    std::cout << "[ NOTE     ] longest value is " << longest
              << " characters, over " << map.size() << " rows\n";
    // A truncated value has text the box will never show.
    EXPECT_EQ(truncated, 0u)
        << truncated << " values do not fit " << translator_logic::MAX_LINES
        << " lines; first such: " << worst;
}

TEST_F(TranslationPipelineTest, NoValueCarriesTheJapaneseIndent) {
    // U+3000 is the source's paragraph indent and converts to CP932 81 40.
    // English does not indent that way, and the load drops it.
    size_t indented = 0;
    for (const auto& kv : map)
        if (kv.second.find("\x81\x40") != std::string::npos) ++indented;
    EXPECT_EQ(indented, 0u);
}

TEST_F(TranslationPipelineTest, NoValueCarriesAnOpenEngineTag) {
    // A <...> pair in English costs the text inside it; a lone '<' is drawn
    // as itself and is not a defect.
    size_t bad = 0;
    std::string first_bad;
    for (const auto& kv : map) {
        const size_t open = kv.second.find('<');
        if (open == std::string::npos) continue;
        if (kv.second.find('>', open + 1) == std::string::npos) continue;
        ++bad;
        if (first_bad.empty()) first_bad = kv.second;
    }
    EXPECT_EQ(bad, 0u) << bad << " values carry a '<' with a '>' after it; "
                                 "first such: '" << first_bad << "'";
}

TEST_F(TranslationPipelineTest, EveryValueIsWellFormedCp932) {
    // A UTF-8 value would reach the renderer as a wrong CJK glyph plus a
    // stray byte.  Dangling lead bytes are what that looks like from here.
    size_t bad = 0;
    std::string first_bad;
    for (const auto& kv : map) {
        const std::string& v = kv.second;
        size_t i = 0;
        bool ok = true;
        while (i < v.size()) {
            unsigned char b = static_cast<unsigned char>(v[i]);
            if (translator_logic::IsCp932LeadByte(b)) {
                unsigned char t = (i + 1 < v.size())
                                  ? static_cast<unsigned char>(v[i + 1]) : 0;
                if (t < 0x40 || t == 0x7F || t > 0xFC) { ok = false; break; }
                i += 2;
            } else {
                ++i;
            }
        }
        if (!ok) { ++bad; if (first_bad.empty()) first_bad = v; }
    }
    EXPECT_EQ(bad, 0u) << bad << " values are not valid CP932; first such: '"
                       << first_bad << "'";
}
