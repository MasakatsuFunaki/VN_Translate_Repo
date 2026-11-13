// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// UT: CP932 lead-byte classification, JP-detection, and UTF-8 <-> CP932 codec.

#include "translator_logic.h"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

using translator_logic::Cp932ToUtf8;
using translator_logic::HasJPLeadByte;
using translator_logic::IsCp932LeadByte;
using translator_logic::Utf8ToCp932;

namespace {

constexpr unsigned char kNullByte = 0x00;
constexpr unsigned char kMaxAscii = 0x7F;
constexpr unsigned char kBelowLeadRange1 = 0x80;
constexpr unsigned char kMinLeadRange1 = 0x81;
constexpr unsigned char kHiraganaLeadByte = 0x82;
constexpr unsigned char kMaxLeadRange1 = 0x9F;
constexpr unsigned char kBelowHalfwidthKana = 0xA0;
constexpr unsigned char kMaxHalfwidthKana = 0xDF;
constexpr unsigned char kMinLeadRange2 = 0xE0;
constexpr unsigned char kMaxLeadRange2 = 0xFC;
constexpr unsigned char kAboveLeadRange2 = 0xFD;
constexpr unsigned char kMaxByte = 0xFF;

#ifdef _WIN32
constexpr UINT kCodePageCp932 = 932;

std::string WideToCp932Oracle(std::wstring_view wide) {
    if (wide.empty()) return {};
    BOOL used_default = FALSE;
    const int size = WideCharToMultiByte(
        kCodePageCp932, WC_NO_BEST_FIT_CHARS,
        wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, &used_default);
    if (size <= 0 || used_default != FALSE) {
        throw std::runtime_error("WideToCp932Oracle conversion failed");
    }
    std::string out(static_cast<std::size_t>(size), '\0');
    const int written = WideCharToMultiByte(
        kCodePageCp932, WC_NO_BEST_FIT_CHARS,
        wide.data(), static_cast<int>(wide.size()),
        &out[0], size, nullptr, &used_default);
    if (written <= 0 || used_default != FALSE) {
        throw std::runtime_error("WideToCp932Oracle buffer write failed");
    }
    return out;
}
#endif

}  // namespace

TEST(Cp932, LeadByteRange) {
    EXPECT_TRUE(IsCp932LeadByte(kMinLeadRange1));
    EXPECT_TRUE(IsCp932LeadByte(kMaxLeadRange1));
    EXPECT_TRUE(IsCp932LeadByte(kMinLeadRange2));
    EXPECT_TRUE(IsCp932LeadByte(kMaxLeadRange2));
    EXPECT_FALSE(IsCp932LeadByte(kBelowLeadRange1));
    EXPECT_FALSE(IsCp932LeadByte(kBelowHalfwidthKana));
    EXPECT_FALSE(IsCp932LeadByte(kMaxHalfwidthKana));
    EXPECT_FALSE(IsCp932LeadByte(kMaxAscii));
}

TEST(Cp932, LeadByteRangeBoundaries) {
    EXPECT_FALSE(IsCp932LeadByte(kNullByte));
    EXPECT_FALSE(IsCp932LeadByte(kMaxByte));
    EXPECT_FALSE(IsCp932LeadByte(kAboveLeadRange2));
}

TEST(HasJpLeadByte, AsciiStringIsRejected) {
    const std::string ascii = "Hello world";
    EXPECT_FALSE(HasJPLeadByte(ascii.data(), ascii.size()));
}

TEST(HasJpLeadByte, EmptyBufferIsRejected) {
    EXPECT_FALSE(HasJPLeadByte("", 0));
}

TEST(HasJpLeadByte, SingleLeadByteWithNoTrailIsRejected) {
    const std::array<char, 1> dangling = {
        static_cast<char>(kHiraganaLeadByte),
    };
    EXPECT_FALSE(HasJPLeadByte(dangling.data(), dangling.size()));
}

#ifdef _WIN32

TEST(HasJpLeadByte, JapaneseStringIsAccepted) {
    const std::string japanese = WideToCp932Oracle(L"あい");
    EXPECT_TRUE(HasJPLeadByte(japanese.data(), japanese.size()));
}

TEST(HasJpLeadByte, JapaneseAfterAsciiPrefix) {
    const std::string mixed = WideToCp932Oracle(L"abcあ");
    EXPECT_TRUE(HasJPLeadByte(mixed.data(), mixed.size()));
}

TEST(Cp932Codec, Utf8ToCp932MatchesWin32OracleForJapaneseText) {
    EXPECT_EQ(Utf8ToCp932("櫻の詩"), WideToCp932Oracle(L"櫻の詩"));
    EXPECT_EQ(Utf8ToCp932("　あい"), WideToCp932Oracle(L"　あい"));
}

TEST(Cp932Codec, Cp932ToUtf8DecodesWin32OracleBytesToLiteralUtf8) {
    const std::string cp932 = WideToCp932Oracle(L"櫻の詩 -Sakura no Uta-");
    EXPECT_EQ(Cp932ToUtf8(cp932), "櫻の詩 -Sakura no Uta-");
}

TEST(Cp932Codec, EmptyAndTruncatedByteArrayDoNotCrash) {
    EXPECT_TRUE(Utf8ToCp932("").empty());
    EXPECT_TRUE(Cp932ToUtf8("").empty());

    const std::array<std::uint8_t, 1> truncated_bytes = {kHiraganaLeadByte};
    const std::string truncated(
        reinterpret_cast<const char*>(truncated_bytes.data()),
        truncated_bytes.size());
    EXPECT_FALSE(Cp932ToUtf8(truncated).empty());
}

#endif
