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
#include <cstdio>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "common/util.h"
#include "test_support.h"

using namespace frat;

namespace {

std::string hex_upper(const Bytes& b) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (std::uint8_t c : b) {
        out += hex[c >> 4];
        out += hex[c & 0xF];
    }
    return out;
}

std::string u4(unsigned v) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%04X", v);
    return buf;
}

std::string u2(unsigned v) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02X", v);
    return buf;
}

}  // namespace

TEST(Cp932, EncodeTableIsPinnedTo9408Codepoints) {
    std::string blob;
    std::size_t n = 0;
    for (unsigned cp = 0; cp < 0x10000; ++cp) {
        const auto enc = utf8_to_cp932_strict(utf8_encode_cp(static_cast<char32_t>(cp)));
        if (!enc) continue;
        ++n;
        blob += u4(cp) + ":" + hex_upper(*enc) + "\n";
    }
    EXPECT_EQ(n, 9408u);
    EXPECT_EQ(frat_test::sha256_hex(blob),
              "f5fefdb9945e3d3960801be3a9d9ad638c5d07ff83392c120637d0a0841464f1");
}

TEST(Cp932, EncodeRejectsBestFitCandidates) {
    // None of these has a CP932 mapping, so the strict encoder must refuse
    // them; WideCharToMultiByte(932, 0, ...) would silently emit 'e'/'c'/'R'.
    for (char32_t cp : {0x00E9u, 0x00A9u, 0x00AEu, 0x00C0u, 0x00FFu})
        EXPECT_FALSE(utf8_to_cp932_strict(utf8_encode_cp(cp)).has_value())
            << "U+" << std::hex << static_cast<unsigned>(cp);
    // ...and maps the six WC_NO_BEST_FIT_CHARS would wrongly reject.
    const std::pair<char32_t, std::uint16_t> mapped[] = {
        {0x00A2, 0x8191}, {0x00A3, 0x8192}, {0x00AC, 0x81CA},
        {0x2016, 0x8161}, {0x2212, 0x817C}, {0x301C, 0x8160},
    };
    for (const auto& [cp, want] : mapped) {
        const auto enc = utf8_to_cp932_strict(utf8_encode_cp(cp));
        ASSERT_TRUE(enc.has_value());
        ASSERT_EQ(enc->size(), 2u);
        EXPECT_EQ(((*enc)[0] << 8) | (*enc)[1], want);
    }
}

TEST(Cp932, DecodeTableIsPinnedIncludingTheSpecialSingles) {
    // Includes the five single bytes a naive Shift-JIS structure test rejects
    // but CP932 maps: 0x80 -> U+0080, 0xA0/0xFD/0xFE/0xFF -> U+F8F0..U+F8F3.
    std::string blob;
    std::size_t singles = 0, pairs = 0;
    for (unsigned b = 0; b < 0x100; ++b) {
        const std::uint8_t by = static_cast<std::uint8_t>(b);
        const auto s = cp932_to_utf8_strict(&by, 1);
        if (!s) continue;
        ++singles;
        std::string cps;
        for (char32_t cp : utf8_decode(*s)) cps += u4(static_cast<unsigned>(cp));
        blob += u2(b) + ">" + cps + "\n";
    }
    std::vector<unsigned> leads, trails;
    for (unsigned v = 0x81; v <= 0x9F; ++v) leads.push_back(v);
    for (unsigned v = 0xE0; v <= 0xFC; ++v) leads.push_back(v);
    for (unsigned v = 0x40; v <= 0x7E; ++v) trails.push_back(v);
    for (unsigned v = 0x80; v <= 0xFC; ++v) trails.push_back(v);
    for (unsigned lead : leads) {
        for (unsigned trail : trails) {
            const std::uint8_t pair[2] = {static_cast<std::uint8_t>(lead),
                                          static_cast<std::uint8_t>(trail)};
            const auto s = cp932_to_utf8_strict(pair, 2);
            if (!s) continue;
            ++pairs;
            std::string cps;
            for (char32_t cp : utf8_decode(*s)) cps += u4(static_cast<unsigned>(cp));
            blob += u2(lead) + u2(trail) + ">" + cps + "\n";
        }
    }
    EXPECT_EQ(singles, 196u);
    EXPECT_EQ(pairs, 9604u);
    EXPECT_EQ(frat_test::sha256_hex(blob),
              "2e1d6b25a4776acaf5403533e37d146e8221d78b534112622ebc4a2b0738a5e4");
}

TEST(Cp932, DecodePuaAndSpecialSingles) {
    const std::uint8_t pua[2] = {0xF0, 0x40};
    EXPECT_EQ(cp932_to_utf8_strict(pua, 2), utf8_encode_cp(0xE000));
    const std::uint8_t a0 = 0xA0;
    EXPECT_EQ(cp932_to_utf8_strict(&a0, 1), utf8_encode_cp(0xF8F0));
}

TEST(Cp932, ReplaceEncodeEmitsQuestionMark) {
    // The lossy encoder substitutes one '?' per unmappable codepoint.
    const Bytes enc = utf8_to_cp932_replace("a\xC3\xA9" "b");  // "aéb"
    ASSERT_EQ(enc.size(), 3u);
    EXPECT_EQ(enc[1], '?');
}
