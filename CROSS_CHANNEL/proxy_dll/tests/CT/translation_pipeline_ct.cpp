// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Translation pipeline component tests for CROSS_CHANNEL.
//
// Reads the staged translations.tsv from disk, validates post-load invariants,
// and exercises the in-place patcher and dialogue-hook lookup paths against
// real table entries.
#include "translator_logic.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cwchar>
#endif

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using translator_logic::HasJPLeadByte;
using translator_logic::IsChoiceLikeJp;
using translator_logic::LoadTsvFile;
using translator_logic::LookupWithStrippedPrefix;
using translator_logic::ParseTsvBuffer;
using translator_logic::PatchTranslationsInPlace;
using translator_logic::TranslationMap;

#ifndef GAME_TSV_PATH
#define GAME_TSV_PATH ""
#endif

namespace {

constexpr std::size_t kMinReasonableMapSize = 10u;
constexpr int kMaxOverBudgetSamples = 200;
constexpr std::size_t kLongEntryThreshold = 14u;
constexpr int kMaxPairs = 4000;

#ifdef _WIN32
constexpr UINT kCodePageCp932 = 932;

std::string to_cp932(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    BOOL used_default = FALSE;
    const int needed = ::WideCharToMultiByte(
        kCodePageCp932, WC_NO_BEST_FIT_CHARS, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, &used_default);
    if (needed <= 0 || used_default != FALSE) {
        throw std::runtime_error("WideCharToMultiByte(932) failed");
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    const int written = ::WideCharToMultiByte(
        kCodePageCp932, WC_NO_BEST_FIT_CHARS, wide.data(),
        static_cast<int>(wide.size()), out.data(), needed, nullptr, &used_default);
    if (written != needed || used_default != FALSE) {
        throw std::runtime_error("WideCharToMultiByte(932) conversion mismatch");
    }
    return out;
}
#endif

bool LoadRealTsv(TranslationMap& out) {
#ifdef _WIN32
    const char* narrow = GAME_TSV_PATH;
    if (narrow == nullptr || *narrow == '\0') {
        return false;
    }
    const int wn =
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, narrow, -1, nullptr, 0);
    if (wn <= 0) {
        return false;
    }
    std::wstring wpath(static_cast<std::size_t>(wn), L'\0');
    const int written =
        ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, narrow, -1, wpath.data(), wn);
    if (written != wn) {
        return false;
    }
    if (!wpath.empty() && wpath.back() == L'\0') {
        wpath.pop_back();
    }

    std::unique_ptr<FILE, decltype(&std::fclose)> f(
        _wfopen(wpath.c_str(), L"rb"), &std::fclose);
    if (!f) {
        return false;
    }
    if (std::fseek(f.get(), 0, SEEK_END) != 0) {
        return false;
    }
    const long sz = std::ftell(f.get());
    if (sz <= 0 || std::fseek(f.get(), 0, SEEK_SET) != 0) {
        return false;
    }
    std::vector<char> buf(static_cast<std::size_t>(sz));
    const std::size_t n = std::fread(buf.data(), 1, buf.size(), f.get());
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
            << "translations.tsv not found or unreadable at " << GAME_TSV_PATH;
    }
};

TEST(LoadTsvFileUtf8Path, OpensNonAsciiUtf8DirectoryPath) {
    namespace fs = std::filesystem;
    struct ScopedTempDir {
        fs::path dir;
        ScopedTempDir() {
            static std::atomic<unsigned int> seq{0};
            dir = fs::temp_directory_path() /
                  fs::u8path("crc_tsv_日本語_" + std::to_string(++seq));
            std::error_code ec;
            fs::remove_all(dir, ec);
            fs::create_directories(dir);
        }
        ~ScopedTempDir() {
            std::error_code ec;
            fs::remove_all(dir, ec);
        }
    };

    ScopedTempDir scratch;
    const fs::path tsv = scratch.dir / "translations.tsv";
    {
        std::ofstream out(tsv, std::ios::binary);
        out << "「おはよう」\t\"Good morning.\"\n";
    }

    TranslationMap loaded;
    ASSERT_TRUE(LoadTsvFile(tsv.u8string(), loaded));
    ASSERT_EQ(loaded.size(), 1u);
    const std::string* en = LookupWithStrippedPrefix(loaded, to_cp932(L"「おはよう」"));
    ASSERT_NE(en, nullptr);
    EXPECT_EQ(*en, "\"Good morning.\"");
}

TEST_F(TranslationPipelineTest, HasReasonableSize) {
    EXPECT_GT(map.size(), kMinReasonableMapSize);
}

TEST_F(TranslationPipelineTest, EveryKeyHasCp932LeadByte) {
    std::size_t bad = 0;
    std::string first_bad;
    for (const auto& kv : map) {
        if (!HasJPLeadByte(kv.first.data(), kv.first.size())) {
            ++bad;
            if (first_bad.empty()) {
                first_bad = kv.first;
            }
        }
    }
    EXPECT_EQ(bad, 0u)
        << bad << " keys lack a CP932 lead byte; first such: '"
        << first_bad << "'";
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

TEST_F(TranslationPipelineTest, OverBudgetEntriesAreSkippedNotTruncated) {
    int sampled = 0;
    int violations = 0;
    std::string first_violation;

    for (const auto& kv : map) {
        const std::string& jp = kv.first;
        const std::string& en = kv.second;
        if (en.size() <= jp.size()) {
            continue;
        }
        if (++sampled > kMaxOverBudgetSamples) {
            break;
        }

        std::vector<unsigned char> buf;
        buf.push_back(0);
        buf.insert(buf.end(), jp.begin(), jp.end());
        buf.push_back(0);

        TranslationMap one{{jp, en}};
        const auto stats = PatchTranslationsInPlace(buf.data(),
                                                    buf.data() + buf.size(), one);

        if (stats.hits != 0 ||
            std::memcmp(buf.data() + 1, jp.data(), jp.size()) != 0) {
            ++violations;
            if (first_violation.empty()) {
                first_violation = jp;
            }
        }
    }
    EXPECT_EQ(violations, 0)
        << violations << " over-budget entries got patched in place "
        "(first: '" << first_violation << "')";
}

TEST_F(TranslationPipelineTest, FieldReportedAnnotationLineResolvesLiterally) {
    const std::string field_prefix = to_cp932(L"※男心を鷲");

    bool any_matched = false;
    for (const auto& kv : map) {
        if (kv.first.size() < field_prefix.size()) {
            continue;
        }
        if (kv.first.compare(0, field_prefix.size(), field_prefix) != 0) {
            continue;
        }
        const std::string* en = LookupWithStrippedPrefix(map, kv.first);
        EXPECT_NE(en, nullptr)
            << "literal lookup failed for key starting with ※男心を鷲";
        any_matched = true;
        break;
    }
    EXPECT_TRUE(any_matched)
        << "no TSV key starts with ※男心を鷲 — extractor regressed back "
        "to the over-aggressive strip OR translations.tsv is stale.";
}

TEST_F(TranslationPipelineTest, FieldReportedSetMealLineResolvesLiterally) {
    const std::string field_prefix = to_cp932(L"Ａ定食は三時間目");

    bool any_matched = false;
    for (const auto& kv : map) {
        if (kv.first.size() < field_prefix.size()) {
            continue;
        }
        if (kv.first.compare(0, field_prefix.size(), field_prefix) != 0) {
            continue;
        }
        const std::string* en = LookupWithStrippedPrefix(map, kv.first);
        EXPECT_NE(en, nullptr)
            << "literal lookup failed for key starting with Ａ定食は三時間目.";
        any_matched = true;
        break;
    }
    EXPECT_TRUE(any_matched)
        << "no TSV key starts with Ａ定食は三時間目 — extractor regressed "
        "back to the over-aggressive strip OR translations.tsv is stale.";
}

TEST_F(TranslationPipelineTest, FallbackStillStripsLeadingDecorator) {
    const std::string stripped_key = to_cp932(L"定食");
    const std::string decorated_key = to_cp932(L"※定食");
    TranslationMap synthetic{{stripped_key, "set meal"}};
    const std::string* en = LookupWithStrippedPrefix(synthetic, decorated_key);
    ASSERT_NE(en, nullptr) << "fallback strip failed on decorated key";
    EXPECT_EQ(*en, "set meal");
}

TEST_F(TranslationPipelineTest, EveryJpResolvesViaForwardLookup) {
    int violations = 0;
    std::string first_violation;
    for (const auto& kv : map) {
        const std::string* en = LookupWithStrippedPrefix(map, kv.first);
        if (en == nullptr || *en != kv.second) {
            ++violations;
            if (first_violation.empty()) {
                first_violation = kv.first;
            }
        }
    }
    EXPECT_EQ(violations, 0)
        << violations << " literal lookups failed (first: '"
        << first_violation << "')";
}

TEST_F(TranslationPipelineTest, ChoiceJpInsideLongerJpDoesNotCorruptLonger) {
    std::vector<const std::string*> longer;
    longer.reserve(map.size());
    for (const auto& kv : map) {
        if (kv.first.size() > kLongEntryThreshold) {
            longer.push_back(&kv.first);
        }
    }

    int pair_tests = 0;
    int violations = 0;
    std::pair<std::string, std::string> first_violation;

    for (const auto& kv : map) {
        if (!IsChoiceLikeJp(kv.first)) {
            continue;
        }
        const std::string& sjp = kv.first;
        for (const std::string* ljp : longer) {
            if (pair_tests >= kMaxPairs) {
                break;
            }
            if (ljp->find(sjp) == std::string::npos) {
                continue;
            }
            ++pair_tests;

            std::vector<unsigned char> buf;
            buf.push_back(0);
            buf.insert(buf.end(), ljp->begin(), ljp->end());
            buf.push_back(0);

            TranslationMap only_short{{sjp, map.at(sjp)}};
            const auto stats = PatchTranslationsInPlace(buf.data(),
                                                        buf.data() + buf.size(),
                                                        only_short);

            if (stats.hits != 0 ||
                std::memcmp(buf.data() + 1, ljp->data(), ljp->size()) != 0) {
                ++violations;
                if (first_violation.first.empty()) {
                    first_violation = {sjp, *ljp};
                }
            }
        }
        if (pair_tests >= kMaxPairs) {
            break;
        }
    }
    EXPECT_EQ(violations, 0)
        << violations << " substring pairs caused longer-JP corruption "
        "(first: short='" << first_violation.first
        << "', long='" << first_violation.second << "')";
    EXPECT_GT(pair_tests, 0)
        << "no (choice-like JP, longer JP containing it) pairs found in "
        "the TSV — test is no-op for this dataset";
}
