// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// CP932 codec unit tests.
//
// Verifies that CP932 single-byte extensions (0x80, 0xA0, 0xFD-0xFF),
// halfwidth katakana, and two-byte characters decode accurately while
// malformed trail bytes are rejected.
#include "common/util.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace crc;

namespace {

constexpr UINT kCodePageCp932 = 932;
constexpr std::uint8_t kSingleByte80 = 0x80;
constexpr std::uint8_t kSingleByteA0 = 0xA0;
constexpr std::uint8_t kSingleByteFd = 0xFD;
constexpr std::uint8_t kSingleByteFe = 0xFE;
constexpr std::uint8_t kSingleByteFf = 0xFF;
constexpr std::uint8_t kOpcodeSampleB5 = 0xB5;
constexpr std::uint8_t kOpcodeSample20 = 0x20;
constexpr std::uint8_t kOpcodeSample03 = 0x03;
constexpr std::uint8_t kAsciiUpperA = 0x41;
constexpr std::uint8_t kLeadByte86 = 0x86;
constexpr std::uint8_t kInvalidTrail3A = 0x3A;
constexpr std::uint8_t kTruncatedLead82 = 0x82;
constexpr std::uint8_t kLeadByte81 = 0x81;
constexpr std::uint8_t kInvalidTrailFd = 0xFD;
constexpr std::size_t kSingleByteLength = 1u;

std::string dec_bytes(const Bytes& b) {
    const auto s = cp932_to_utf8_strict(b);
    return s ? *s : std::string("<INVALID>");
}

std::string dec(std::initializer_list<std::uint8_t> bytes) {
    return dec_bytes(Bytes(bytes));
}

bool valid(std::initializer_list<std::uint8_t> bytes) {
    const Bytes b(bytes);
    return cp932_to_utf8_strict(b).has_value();
}

Bytes to_sjis(std::wstring_view wide) {
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
    Bytes out(static_cast<std::size_t>(needed));
    const int written = ::WideCharToMultiByte(
        kCodePageCp932, WC_NO_BEST_FIT_CHARS, wide.data(),
        static_cast<int>(wide.size()), reinterpret_cast<char*>(out.data()),
        needed, nullptr, &used_default);
    if (written != needed || used_default != FALSE) {
        throw std::runtime_error("WideCharToMultiByte(932) conversion mismatch");
    }
    return out;
}

}  // namespace

TEST(Cp932, decodes_special_single_bytes) {
    EXPECT_EQ(dec({kSingleByte80}), "\xC2\x80");      // U+0080
    EXPECT_EQ(dec({kSingleByteA0}), "\xEF\xA3\xB0");  // U+F8F0
    EXPECT_EQ(dec({kSingleByteFd}), "\xEF\xA3\xB1");  // U+F8F1
    EXPECT_EQ(dec({kSingleByteFe}), "\xEF\xA3\xB2");  // U+F8F2
    EXPECT_EQ(dec({kSingleByteFf}), "\xEF\xA3\xB3");  // U+F8F3
}

TEST(Cp932, special_bytes_survive_inside_a_run) {
    EXPECT_TRUE(valid({kOpcodeSampleB5, kSingleByteFf, kSingleByteFf,
                       kSingleByteFf, kOpcodeSample20, kOpcodeSample03}));
    EXPECT_EQ(dec({'a', kSingleByteFf, 'b'}), "a\xEF\xA3\xB3" "b");

    Bytes adjacent = to_sjis(L"あ");
    adjacent.push_back(kSingleByteFf);
    EXPECT_EQ(dec_bytes(adjacent), "あ\xEF\xA3\xB3");
}

TEST(Cp932, a0_does_not_swallow_the_following_byte) {
    EXPECT_EQ(dec({kSingleByteA0, kAsciiUpperA}), "\xEF\xA3\xB0" "A");
}

TEST(Cp932, halfwidth_katakana_singles) {
    const Bytes stop = to_sjis(L"｡");
    const Bytes semi_voiced = to_sjis(L"ﾟ");
    ASSERT_EQ(stop.size(), kSingleByteLength);
    ASSERT_EQ(semi_voiced.size(), kSingleByteLength);
    EXPECT_EQ(dec_bytes(stop), "｡");
    EXPECT_EQ(dec_bytes(semi_voiced), "ﾟ");
}

TEST(Cp932, decodes_ordinary_two_byte_characters) {
    EXPECT_EQ(dec_bytes(to_sjis(L"あ")), "あ");
    EXPECT_EQ(dec_bytes(to_sjis(L"「")), "「");
    EXPECT_EQ(dec_bytes(to_sjis(L"日")), "日");
}

TEST(Cp932, rejects_bad_trail_bytes) {
    EXPECT_FALSE(valid({kLeadByte86, kInvalidTrail3A}));
    EXPECT_FALSE(valid({kTruncatedLead82}));
    EXPECT_FALSE(valid({kLeadByte81, kInvalidTrailFd}));
}

TEST(Cp932, empty_input_is_the_empty_string) {
    EXPECT_EQ(dec({}), "");
}

TEST(Cp932, replace_mode_keeps_special_singles) {
    const Bytes b{kSingleByteFf, kLeadByte86, kInvalidTrail3A};
    const std::string s = cp932_to_utf8_replace(b.data(), b.size());
    EXPECT_EQ(s.rfind("\xEF\xA3\xB3", 0), 0u) << "0xFF should decode, not be replaced";
    EXPECT_NE(s.find("\xEF\xBF\xBD"), std::string::npos) << "0x86 0x3A should be replaced";
}

TEST(Cp932, utf8_to_cp932_strict_rejects_invalid_utf8_and_preserves_empty) {
    const auto empty_encoded = utf8_to_cp932_strict("");
    ASSERT_TRUE(empty_encoded.has_value());
    EXPECT_TRUE(empty_encoded->empty());

    const auto valid_encoded = utf8_to_cp932_strict("あ");
    ASSERT_TRUE(valid_encoded.has_value());
    EXPECT_EQ(*valid_encoded, to_sjis(L"あ"));

    EXPECT_FALSE(utf8_to_cp932_strict("\xFF").has_value());
    EXPECT_FALSE(utf8_to_cp932_strict("\xE3\x81").has_value());
}
