// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// CP932 codec and lead-byte unit tests for CROSS_CHANNEL.
#include "translator_logic.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

using translator_logic::HasJPLeadByte;
using translator_logic::IsCp932LeadByte;

namespace {

constexpr UINT kCodePageCp932 = 932;
constexpr unsigned char kLeadRange1First = 0x81;
constexpr unsigned char kLeadRange1Last = 0x9F;
constexpr unsigned char kLeadRange2First = 0xE0;
constexpr unsigned char kLeadRange2Last = 0xFC;
constexpr unsigned char kSingleByte80 = 0x80;
constexpr unsigned char kSingleByteA0 = 0xA0;
constexpr unsigned char kHalfwidthKanaLast = 0xDF;
constexpr unsigned char kAsciiLast = 0x7F;
constexpr unsigned char kNullByte = 0x00;
constexpr unsigned char kSingleByteFd = 0xFD;
constexpr unsigned char kSingleByteFf = 0xFF;
constexpr unsigned char kTruncatedHiraganaLead = 0x82;
constexpr std::size_t kSingleByteCount = 1u;

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

}  // namespace

TEST(Cp932, LeadByteRange) {
    EXPECT_TRUE(IsCp932LeadByte(kLeadRange1First));
    EXPECT_TRUE(IsCp932LeadByte(kLeadRange1Last));
    EXPECT_TRUE(IsCp932LeadByte(kLeadRange2First));
    EXPECT_TRUE(IsCp932LeadByte(kLeadRange2Last));
    EXPECT_FALSE(IsCp932LeadByte(kSingleByte80));
    EXPECT_FALSE(IsCp932LeadByte(kSingleByteA0));
    EXPECT_FALSE(IsCp932LeadByte(kHalfwidthKanaLast));
    EXPECT_FALSE(IsCp932LeadByte(kAsciiLast));
}

TEST(Cp932, LeadByteRangeBoundaries) {
    EXPECT_FALSE(IsCp932LeadByte(kNullByte));
    EXPECT_FALSE(IsCp932LeadByte(kSingleByteFf));
    EXPECT_FALSE(IsCp932LeadByte(kSingleByteFd));
}

TEST(HasJpLeadByte, AsciiStringIsRejected) {
    const std::string ascii = "Hello world";
    EXPECT_FALSE(HasJPLeadByte(ascii.data(), ascii.size()));
}

TEST(HasJpLeadByte, JapaneseStringIsAccepted) {
    const std::string japanese = to_cp932(L"あい");
    EXPECT_TRUE(HasJPLeadByte(japanese.data(), japanese.size()));
}

TEST(HasJpLeadByte, EmptyBufferIsRejected) {
    EXPECT_FALSE(HasJPLeadByte("", 0));
}

TEST(HasJpLeadByte, SingleLeadByteWithNoTrailIsRejected) {
    const char dangling[kSingleByteCount] = {static_cast<char>(kTruncatedHiraganaLead)};
    EXPECT_FALSE(HasJPLeadByte(dangling, kSingleByteCount));
}

TEST(HasJpLeadByte, JapaneseAfterAsciiPrefix) {
    const std::string mixed = "abc" + to_cp932(L"あ");
    EXPECT_TRUE(HasJPLeadByte(mixed.data(), mixed.size()));
}
