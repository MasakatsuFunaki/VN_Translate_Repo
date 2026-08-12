// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// UT — in-place TSV patcher (translator_logic::PatchTranslationsInPlace).
//
// Covers the exact pattern that caused visible mid-dialogue corruption in
// the field: a short choice-likely JP entry (10..14 bytes) appearing as a
// SUFFIX or substring of a longer, untranslated dialogue JP entry. The
// previous patcher had two paths that would corrupt the longer entry:
//
//   1. A "FORCED pass" that searched for choice-likely JP needles ANYWHERE
//      in the buffer and overwrote without validity checks.
//   2. An `is_choice_like` override on the main pass that relaxed the
//      "byte before match must be null" rule for the same range.
//
// Both are removed; the regression tests here lock that down. Choice menu
// rendering is now handled at the engine's choice-opcode entry point
// (C_PreChoiceHook) so the in-place patch never needs to "win the race"
// against substring matches inside dialogue lines.

#include "translator_logic.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

using translator_logic::TranslationMap;
using translator_logic::PatchTranslationsInPlace;
using translator_logic::IsChoiceLikeJp;
using translator_logic::FitToSlot;

namespace {

// Helper: build a buffer from a sequence of (bytes, append_null) pairs.
// Returns a contiguous vector that we can hand to the patcher.
std::vector<unsigned char> MakeBuf(std::initializer_list<std::string> records) {
    std::vector<unsigned char> v;
    for (const auto& r : records) {
        v.insert(v.end(), r.begin(), r.end());
        v.push_back(0);   // null-terminator between records
    }
    return v;
}

// Helper: pull the bytes [offset..offset+len) out of `buf` as a string.
std::string Slice(const std::vector<unsigned char>& buf,
                   size_t offset, size_t len) {
    return std::string(buf.begin() + offset, buf.begin() + offset + len);
}

}  // namespace

// ─────────────────────────── happy paths ─────────────────────────────────

TEST(Patcher, StandaloneJpBetweenNullsGetsPatched) {
    // [\0][JP\0]  — JP preceded by null, followed by null.
    auto buf = MakeBuf({"", "JP_KEY", ""});
    TranslationMap m{ {"JP_KEY", "EN"} };

    auto stats = PatchTranslationsInPlace(buf.data(), buf.data() + buf.size(), m);
    EXPECT_EQ(stats.hits, 1);
    EXPECT_EQ(stats.too_long, 0);
    // JP_KEY (6 bytes) + null at +6. EN is "EN" (2) + 4 spaces (slot pad) + null.
    EXPECT_EQ(Slice(buf, 1, 2), "EN");
    EXPECT_EQ(buf[7], 0);  // null preserved
}

TEST(Patcher, JpAtStartOfBufferIsValidStart) {
    auto buf = MakeBuf({"JP_KEY", ""});
    TranslationMap m{ {"JP_KEY", "EN"} };

    auto stats = PatchTranslationsInPlace(buf.data(), buf.data() + buf.size(), m);
    EXPECT_EQ(stats.hits, 1);
    EXPECT_EQ(Slice(buf, 0, 2), "EN");
}

TEST(Patcher, EnSameSizeAsJpFitsExactly) {
    auto buf = MakeBuf({"", "ABCDE", ""});
    TranslationMap m{ {"ABCDE", "12345"} };

    auto stats = PatchTranslationsInPlace(buf.data(), buf.data() + buf.size(), m);
    EXPECT_EQ(stats.hits, 1);
    EXPECT_EQ(Slice(buf, 1, 5), "12345");
}

TEST(Patcher, EnLongerThanJpStealsTrailingNulls) {
    // [\0][JP\0\0\0\0]  — 4 trailing null bytes available to steal.
    std::vector<unsigned char> buf = {
        0, 'J','P', 0, 0, 0, 0
    };
    TranslationMap m{ {"JP", "WIDER"} };  // 5 bytes vs 2 in slot

    auto stats = PatchTranslationsInPlace(buf.data(), buf.data() + buf.size(), m);
    EXPECT_EQ(stats.hits, 1);
    EXPECT_EQ(0, std::memcmp(buf.data() + 1, "WIDER", 5));
}

// ─────────────────────────── regression cases ────────────────────────────

TEST(Patcher, ChoiceLikeJpAsSuffixOfLongerJpIsNotPatched_Regression) {
    // The field bug: a 10-byte choice JP that's also the suffix of a
    // longer dialogue JP got the longer line spliced with truncated EN
    // (visible in-game as e.g. "<japanese prefix>Oops."). This test
    // wires up exactly that pattern: `suffix` is a choice-likely TSV
    // entry; `long_jp` has those same 10 bytes as its suffix. We do
    // NOT put `long_jp` in the translation map — the test asserts that
    // the suffix-match in the buffer doesn't fire and the bytes for
    // `long_jp` are left byte-for-byte intact.
    const std::string suffix(10, 'X');                   // 10-byte choice-like
    const std::string long_jp = "PREFIX_BYTES" + suffix; // 22 bytes total

    ASSERT_TRUE(IsChoiceLikeJp(suffix));
    ASSERT_FALSE(IsChoiceLikeJp(long_jp));

    auto buf = MakeBuf({"", long_jp});
    TranslationMap m{ {suffix, "Oops."} };  // ONLY the short choice entry

    auto stats = PatchTranslationsInPlace(buf.data(), buf.data() + buf.size(), m);

    // suffix occurs in the buffer as the LAST 10 bytes of long_jp.
    // validEnd is true (null after) but validStart is false (preceded
    // by 'S' from "PREFIX_BYTES"), so the patcher must skip. long_jp
    // must remain untouched.
    EXPECT_EQ(stats.hits, 0)
        << "suffix-match should not fire when validStart is false";
    EXPECT_EQ(0, std::memcmp(buf.data() + 1, long_jp.data(), long_jp.size()))
        << "longer JP got corrupted by a substring-suffix match";
}

TEST(Patcher, ChoiceLikeJpInsideLongerJpIsNotPatched) {
    // SUFFIX_JP appears in the MIDDLE of long_jp. Both validEnd (next
    // byte is more JP, not null) and validStart (prev byte is JP, not
    // null) are false — patcher must skip.
    const std::string short_jp(10, 'M');   // choice-like
    const std::string long_jp = "AAA" + short_jp + "BBB";

    auto buf = MakeBuf({"", long_jp});
    TranslationMap m{ {short_jp, "ShortEN"} };

    auto stats = PatchTranslationsInPlace(buf.data(), buf.data() + buf.size(), m);
    EXPECT_EQ(stats.hits, 0);
    EXPECT_EQ(0, std::memcmp(buf.data() + 1, long_jp.data(), long_jp.size()));
}

TEST(Patcher, ShortJpFollowedByMoreJpIsNotPatched) {
    // Same idea but with a non-choice-like short JP. Catches the
    // pre-existing "JP=た hits tail of あなた\0" class of bugs.
    // (UTF-8: あなた = 9 bytes, た = 3 bytes — sits as the last 3 bytes
    // of あなた.)
    auto buf = MakeBuf({"あなた"});
    TranslationMap m{ {"た", "T"} };

    auto stats = PatchTranslationsInPlace(buf.data(), buf.data() + buf.size(), m);
    // "た" appears as the last 3 bytes of あなた, validEnd true (null
    // at +9), validStart false (preceded by "な" trail bytes). Skip.
    EXPECT_EQ(stats.hits, 0);
    EXPECT_EQ(Slice(buf, 0, 9), "あなた");
}

TEST(Patcher, DialogueLengthEnTooLongIsCountedAndNotWritten) {
    // A long JP whose EN exceeds the slot byte-width is skipped (no
    // truncation in place — the render-time hook handles that). The
    // bytes must be untouched.
    const std::string jp = "JP_KEY";              // 6 bytes (not choice-like)
    const std::string en = "this EN is way longer than the slot"; // way > 6

    auto buf = MakeBuf({"", jp, ""});
    TranslationMap m{ {jp, en} };

    auto stats = PatchTranslationsInPlace(buf.data(), buf.data() + buf.size(), m);
    EXPECT_EQ(stats.hits, 0);
    EXPECT_EQ(stats.too_long, 1);
    EXPECT_EQ(Slice(buf, 1, 6), jp);
}

TEST(Patcher, ChoiceLikeJpTooLongEnIsSkipped_Regression) {
    // Field bug: a 12-byte JP whose EN didn't fit ("風が吹いた。" → "The
    // wind blew.") used to be FitToSlot-truncated to "The wind" + space
    // padding and written into sn.bin. The dialogue hook later read the
    // truncated EN as the lookup KEY, missed (TSV key was JP), and the
    // engine rendered "The wind" with no further substitution. Now the
    // patcher SKIPS over-budget entries and leaves the JP intact so the
    // dialogue hook sees it and can substitute with the full EN.
    const std::string jp(12, 'D');                  // 12-byte choice-like
    const std::string en = "Go up to the roof";     // 17 bytes (too long)

    auto buf = MakeBuf({"", jp, ""});
    TranslationMap m{ {jp, en} };

    auto stats = PatchTranslationsInPlace(buf.data(), buf.data() + buf.size(), m);
    EXPECT_EQ(stats.hits, 0);
    EXPECT_EQ(stats.too_long, 1);
    // JP bytes must be untouched so the dialogue hook can match it.
    EXPECT_EQ(Slice(buf, 1, jp.size()), jp);
}

// ─────────────────────────── helper-level checks ─────────────────────────

TEST(Patcher_Helpers, IsChoiceLikeJpRange) {
    EXPECT_FALSE(IsChoiceLikeJp(""));
    EXPECT_FALSE(IsChoiceLikeJp(std::string(9,  'x')));
    EXPECT_TRUE (IsChoiceLikeJp(std::string(10, 'x')));
    EXPECT_TRUE (IsChoiceLikeJp(std::string(14, 'x')));
    EXPECT_FALSE(IsChoiceLikeJp(std::string(15, 'x')));
}

TEST(Patcher_Helpers, FitToSlotPrefersLastSpace) {
    EXPECT_EQ(FitToSlot("Go up to the roof", 10), "Go up to");
    EXPECT_EQ(FitToSlot("Talk to her", 10), "Talk to");
}

TEST(Patcher_Helpers, FitToSlotHardClipsWhenNoSpaceFits) {
    EXPECT_EQ(FitToSlot("LongRunOfChars", 5), "LongR");
}

TEST(Patcher_Helpers, FitToSlotShortInputPassesThrough) {
    EXPECT_EQ(FitToSlot("hi", 10), "hi");
}
