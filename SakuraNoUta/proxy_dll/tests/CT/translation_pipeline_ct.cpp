// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Component tests: end-to-end load of the deployed translations.tsv and
// validation of post-load runtime invariants.

#include "translator_logic.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdio>
#include <cwchar>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using translator_logic::HasJPLeadByte;
using translator_logic::ParseTsvBuffer;
using translator_logic::TranslationMap;

#ifndef GAME_TSV_PATH
#  define GAME_TSV_PATH ""
#endif

namespace {

constexpr std::size_t kMinExpectedTableRows = 10;
constexpr unsigned char kMinCp932Trail = 0x40;
constexpr unsigned char kAsciiDelete = 0x7F;
constexpr unsigned char kMaxCp932Trail = 0xFC;
constexpr std::size_t kCp932PairBytes = 2;

bool LoadRealTsv(TranslationMap& out) {
#ifdef _WIN32
    const char* const narrow = GAME_TSV_PATH;
    if (narrow == nullptr || *narrow == '\0') {
        return false;
    }
    const int wn =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, narrow, -1, nullptr, 0);
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

    std::unique_ptr<std::FILE, decltype(&std::fclose)> file(
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
    EXPECT_GT(map.size(), kMinExpectedTableRows);
}

TEST_F(TranslationPipelineTest, EveryKeyHasCp932LeadByte) {
    std::size_t bad = 0;
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
    std::size_t bad = 0;
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
    std::size_t widest = 0;
    std::string worst;
    for (const auto& kv : map) {
        const std::string text =
            translator_logic::WrapForMessageBox(kv.second).text;
        std::size_t pos = 0;
        for (;;) {
            const std::size_t nl = text.find('\n', pos);
            const std::size_t width =
                (nl == std::string::npos ? text.size() : nl) - pos;
            if (width > widest) {
                widest = width;
                worst = kv.second;
            }
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
    std::size_t truncated = 0;
    std::size_t longest = 0;
    std::string worst;
    for (const auto& kv : map) {
        const translator_logic::WrappedText w =
            translator_logic::WrapForMessageBox(kv.second);
        EXPECT_LE(w.lines, translator_logic::MAX_LINES);
        if (w.truncated) {
            ++truncated;
            if (worst.empty()) worst = kv.second;
        }
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
    // U+3000 is the source's paragraph indent and converts to CP932 0x81 0x40.
    // English does not indent that way, and the load drops it.
    const std::string jp_indent = translator_logic::Utf8ToCp932("　");
    ASSERT_FALSE(jp_indent.empty());
    std::size_t indented = 0;
    for (const auto& kv : map) {
        if (kv.second.find(jp_indent) != std::string::npos) ++indented;
    }
    EXPECT_EQ(indented, 0u);
}

TEST_F(TranslationPipelineTest, NoValueCarriesAnOpenEngineTag) {
    // A <...> pair in English costs the text inside it; a lone '<' is drawn
    // as itself and is harmless.
    std::size_t bad = 0;
    std::string first_bad;
    for (const auto& kv : map) {
        const std::size_t open = kv.second.find('<');
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
    std::size_t bad = 0;
    std::string first_bad;
    for (const auto& kv : map) {
        const std::string& v = kv.second;
        std::size_t i = 0;
        bool ok = true;
        while (i < v.size()) {
            const unsigned char b = static_cast<unsigned char>(v[i]);
            if (!translator_logic::IsCp932LeadByte(b)) {
                ++i;
                continue;
            }
            const unsigned char trail =
                (i + 1 < v.size()) ? static_cast<unsigned char>(v[i + 1]) : 0;
            if (trail < kMinCp932Trail || trail == kAsciiDelete ||
                trail > kMaxCp932Trail) {
                ok = false;
                break;
            }
            i += kCp932PairBytes;
        }
        if (!ok) {
            ++bad;
            if (first_bad.empty()) first_bad = v;
        }
    }
    EXPECT_EQ(bad, 0u) << bad << " values are not valid CP932; first such: '"
                       << first_bad << "'";
}
