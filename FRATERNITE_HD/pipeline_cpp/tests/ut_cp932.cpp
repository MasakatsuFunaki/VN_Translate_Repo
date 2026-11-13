// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// The full CP932 codec, pinned by digest so a table change cannot slip in
// unnoticed.
//
// The encoder is the risky direction: WideCharToMultiByte(932) best-fits 458
// BMP codepoints that CP932 cannot represent, and WC_NO_BEST_FIT_CHARS
// overcorrects, so util.cpp builds its own table by inverting the decoder.
#include "common/util.h"
#include "test_support.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

using namespace frat;

namespace {

constexpr unsigned int kNibbleShift = 4;
constexpr std::uint8_t kLowNibbleMask = 0xFu;
constexpr std::size_t kHexFmtBufSize = 8;
constexpr unsigned int kBmpCodepointLimit = 0x10000u;
constexpr std::size_t kExpectedEncodedCodepoints = 9408u;

constexpr char32_t kLatinSmallEAcute = 0x00E9u;
constexpr char32_t kCopyrightSign = 0x00A9u;
constexpr char32_t kRegisteredSign = 0x00AEu;
constexpr char32_t kLatinCapitalAGrave = 0x00C0u;
constexpr char32_t kLatinSmallYDiaeresis = 0x00FFu;

constexpr char32_t kCentSign = 0x00A2u;
constexpr std::uint16_t kCp932Cent = 0x8191u;
constexpr char32_t kPoundSign = 0x00A3u;
constexpr std::uint16_t kCp932Pound = 0x8192u;
constexpr char32_t kNotSign = 0x00ACu;
constexpr std::uint16_t kCp932Not = 0x81CAu;
constexpr char32_t kDoubleVerticalLine = 0x2016u;
constexpr std::uint16_t kCp932DoubleVerticalLine = 0x8161u;
constexpr char32_t kMinusSign = 0x2212u;
constexpr std::uint16_t kCp932MinusSign = 0x817Cu;
constexpr char32_t kWaveDash = 0x301Cu;
constexpr std::uint16_t kCp932WaveDash = 0x8160u;

constexpr std::size_t kCp932PairBytes = 2u;
constexpr unsigned int kBitsPerByte = 8u;
constexpr unsigned int kSingleByteLimit = 0x100u;
constexpr unsigned int kLeadRange1First = 0x81u;
constexpr unsigned int kLeadRange1Last = 0x9Fu;
constexpr unsigned int kLeadRange2First = 0xE0u;
constexpr unsigned int kLeadRange2Last = 0xFCu;
constexpr unsigned int kTrailRange1First = 0x40u;
constexpr unsigned int kTrailRange1Last = 0x7Eu;
constexpr unsigned int kTrailRange2First = 0x80u;
constexpr unsigned int kTrailRange2Last = 0xFCu;
constexpr std::size_t kExpectedDecodedSingles = 196u;
constexpr std::size_t kExpectedDecodedPairs = 9604u;

constexpr std::uint8_t kPuaLeadFirst = 0xF0u;
constexpr std::uint8_t kPuaTrailFirst = 0x40u;
constexpr char32_t kPuaFirstCodepoint = 0xE000u;
constexpr std::uint8_t kSpecialSingleA0 = 0xA0u;
constexpr char32_t kApplePuaA0Codepoint = 0xF8F0u;
constexpr std::size_t kExpectedReplaceEncodedLen = 3u;
constexpr std::size_t kMiddleReplacedByteIndex = 1u;

std::string hex_upper(const Bytes& b) {
    static const char* const kHexDigits = "0123456789ABCDEF";
    std::string out;
    for (std::uint8_t c : b) {
        out += kHexDigits[c >> kNibbleShift];
        out += kHexDigits[c & kLowNibbleMask];
    }
    return out;
}

std::string u4(unsigned v) {
    char buf[kHexFmtBufSize];
    std::snprintf(buf, sizeof(buf), "%04X", v);
    return buf;
}

std::string u2(unsigned v) {
    char buf[kHexFmtBufSize];
    std::snprintf(buf, sizeof(buf), "%02X", v);
    return buf;
}

}  // namespace

TEST(Cp932, EncodeTableIsPinnedTo9408Codepoints) {
    std::string blob;
    std::size_t n = 0;
    for (unsigned cp = 0; cp < kBmpCodepointLimit; ++cp) {
        const auto enc = utf8_to_cp932_strict(utf8_encode_cp(static_cast<char32_t>(cp)));
        if (!enc) {
            continue;
        }
        ++n;
        blob += u4(cp) + ":" + hex_upper(*enc) + "\n";
    }
    EXPECT_EQ(n, kExpectedEncodedCodepoints);
    EXPECT_EQ(frat_test::sha256_hex(blob),
              "f5fefdb9945e3d3960801be3a9d9ad638c5d07ff83392c120637d0a0841464f1");
}

TEST(Cp932, EncodeRejectsBestFitCandidates) {
    // None of these has a CP932 mapping, so the strict encoder must refuse
    // them; WideCharToMultiByte(932, 0, ...) would silently emit 'e'/'c'/'R'.
    for (char32_t cp : {kLatinSmallEAcute, kCopyrightSign, kRegisteredSign,
                        kLatinCapitalAGrave, kLatinSmallYDiaeresis}) {
        EXPECT_FALSE(utf8_to_cp932_strict(utf8_encode_cp(cp)).has_value())
            << "U+" << std::hex << static_cast<unsigned>(cp);
    }
    // ...and maps the six WC_NO_BEST_FIT_CHARS would wrongly reject.
    const std::pair<char32_t, std::uint16_t> mapped[] = {
        {kCentSign, kCp932Cent},
        {kPoundSign, kCp932Pound},
        {kNotSign, kCp932Not},
        {kDoubleVerticalLine, kCp932DoubleVerticalLine},
        {kMinusSign, kCp932MinusSign},
        {kWaveDash, kCp932WaveDash},
    };
    for (const auto& [cp, want] : mapped) {
        const auto enc = utf8_to_cp932_strict(utf8_encode_cp(cp));
        ASSERT_TRUE(enc.has_value());
        ASSERT_EQ(enc->size(), kCp932PairBytes);
        EXPECT_EQ((static_cast<unsigned>((*enc)[0]) << kBitsPerByte) | (*enc)[1], want);
    }
}

TEST(Cp932, DecodeTableIsPinnedIncludingTheSpecialSingles) {
    // Includes the five single bytes a naive Shift-JIS structure test rejects
    // but CP932 maps: 0x80 -> U+0080, 0xA0/0xFD/0xFE/0xFF -> U+F8F0..U+F8F3.
    std::string blob;
    std::size_t singles = 0;
    std::size_t pairs = 0;
    for (unsigned b = 0; b < kSingleByteLimit; ++b) {
        const std::uint8_t by = static_cast<std::uint8_t>(b);
        const auto s = cp932_to_utf8_strict(&by, 1);
        if (!s) {
            continue;
        }
        ++singles;
        std::string cps;
        for (char32_t cp : utf8_decode(*s)) {
            cps += u4(static_cast<unsigned>(cp));
        }
        blob += u2(b) + ">" + cps + "\n";
    }
    std::vector<unsigned> leads;
    std::vector<unsigned> trails;
    for (unsigned v = kLeadRange1First; v <= kLeadRange1Last; ++v) {
        leads.push_back(v);
    }
    for (unsigned v = kLeadRange2First; v <= kLeadRange2Last; ++v) {
        leads.push_back(v);
    }
    for (unsigned v = kTrailRange1First; v <= kTrailRange1Last; ++v) {
        trails.push_back(v);
    }
    for (unsigned v = kTrailRange2First; v <= kTrailRange2Last; ++v) {
        trails.push_back(v);
    }
    for (unsigned lead : leads) {
        for (unsigned trail : trails) {
            const std::uint8_t pair[kCp932PairBytes] = {
                static_cast<std::uint8_t>(lead),
                static_cast<std::uint8_t>(trail),
            };
            const auto s = cp932_to_utf8_strict(pair, kCp932PairBytes);
            if (!s) {
                continue;
            }
            ++pairs;
            std::string cps;
            for (char32_t cp : utf8_decode(*s)) {
                cps += u4(static_cast<unsigned>(cp));
            }
            blob += u2(lead) + u2(trail) + ">" + cps + "\n";
        }
    }
    EXPECT_EQ(singles, kExpectedDecodedSingles);
    EXPECT_EQ(pairs, kExpectedDecodedPairs);
    EXPECT_EQ(frat_test::sha256_hex(blob),
              "2e1d6b25a4776acaf5403533e37d146e8221d78b534112622ebc4a2b0738a5e4");
}

TEST(Cp932, DecodePuaAndSpecialSingles) {
    const std::uint8_t pua[kCp932PairBytes] = {kPuaLeadFirst, kPuaTrailFirst};
    EXPECT_EQ(cp932_to_utf8_strict(pua, kCp932PairBytes), utf8_encode_cp(kPuaFirstCodepoint));
    const std::uint8_t a0 = kSpecialSingleA0;
    EXPECT_EQ(cp932_to_utf8_strict(&a0, 1), utf8_encode_cp(kApplePuaA0Codepoint));
}

TEST(Cp932, ReplaceEncodeEmitsQuestionMark) {
    // The lossy encoder substitutes one '?' per unmappable codepoint.
    const Bytes enc = utf8_to_cp932_replace("aéb");
    ASSERT_EQ(enc.size(), kExpectedReplaceEncodedLen);
    EXPECT_EQ(enc[kMiddleReplacedByteIndex], '?');
}
