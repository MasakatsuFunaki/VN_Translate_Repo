// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// CT — Translation pipeline component tests for CROSS_CHANNEL.
//
// End-to-end: read the deployed translations.tsv from disk, parse it,
// validate post-load invariants the rest of the DLL depends on, AND
// exercise the patcher + dialogue-hook lookup paths against the real
// data. SKIPs cleanly if the TSV doesn't exist (machine-dependent file).
//
// Every behavioural test in this file was added because a real bug
// reached the field that pure UT-level coverage didn't catch — they
// guard the integration boundary between the TSV data and the runtime
// hooks. If you add a new behaviour to the patcher or lookup path, add
// a TSV-walking invariant here too.

#include "translator_logic.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
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
using translator_logic::HasJPLeadByte;
using translator_logic::IsChoiceLikeJp;
using translator_logic::PatchTranslationsInPlace;
using translator_logic::LookupWithStrippedPrefix;

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

// ─────────── Behaviour tests against the real TSV ──────────────────────
// Each test below corresponds to a bug that reached the field. Failures
// here mean the same class of corruption is back. Don't relax these
// without adding an alternative defence.

// REGRESSION: 10..14-byte JP whose EN didn't fit was truncated in-place
// (PickEnFit + FitToSlot) and the dialogue hook later read the truncated
// EN as the JP key, missed, and the engine rendered "The wind" instead
// of "The wind blew." Now the patcher must SKIP every over-budget entry
// and leave the JP intact for the render-time hook to substitute.
TEST_F(TranslationPipelineTest, OverBudgetEntriesAreSkippedNotTruncated) {
    // Sample a slice — full TSV has tens of thousands of entries; running
    // the patcher per-entry on the full set is O(N) and fine, but the
    // useful signal is captured by the first ~200 over-budget entries.
    int sampled = 0;
    int violations = 0;
    std::string first_violation;

    for (const auto& kv : map) {
        const std::string& jp = kv.first;
        const std::string& en = kv.second;
        if (en.size() <= jp.size()) continue;       // not over-budget
        if (++sampled > 200) break;

        // [\0][JP\0]  — null on both sides so the only way the patcher
        // would write here is if it ignored the over-budget condition.
        std::vector<unsigned char> buf;
        buf.push_back(0);
        buf.insert(buf.end(), jp.begin(), jp.end());
        buf.push_back(0);

        TranslationMap one{ {jp, en} };
        auto stats = PatchTranslationsInPlace(buf.data(),
                                                buf.data() + buf.size(), one);

        if (stats.hits != 0 ||
            std::memcmp(buf.data() + 1, jp.data(), jp.size()) != 0) {
            ++violations;
            if (first_violation.empty()) first_violation = jp;
        }
    }
    EXPECT_EQ(violations, 0)
        << violations << " over-budget entries got patched in place "
        "(first: '" << first_violation << "')";
}

// REGRESSION (literal byte-path): the exact JP the field bug surfaced.
// History: ※-prefixed dialogue lines rendered raw because the OLD
// the extract step's control-prefix strip dropped the leading ※ before the
// TSV was built. Engine still emits ※ at runtime → literal lookup
// missed → JP rendered raw.  The narrowed extractor (CLAUDE.md §6)
// now keeps the ※, so the engine bytes match a TSV key VERBATIM.
//
// Ground truth for the byte sequence comes from an actual PRE-LOOP MISS
// log line captured in the field, NOT from a from-memory CP932 table:
//
//   PRE-LOOP MISS #15: len=74 bytes=[81 A6 92 6A 90 53 82 F0 98 68]
//                                    ^─── ※ ───^^^^^─── 男 ──^^^^^─── 心 …
//
// An earlier version of this file used 0x81 0xA8 (which is ★) and
// passed because the production code used the same wrong constant —
// we now tie the CT byte sequence to the in-game MISS log so that
// kind of mistake fails here loudly.
TEST_F(TranslationPipelineTest, FieldReportedAnnotationLineResolvesLiterally) {
    static const unsigned char kFieldBytes[] = {
        0x81, 0xA6,  // ※
        0x92, 0x6A,  // 男
        0x90, 0x53,  // 心
        0x82, 0xF0,  // を
        0x98, 0x68,  // 鷲
    };
    std::string field_prefix(reinterpret_cast<const char*>(kFieldBytes),
                              sizeof(kFieldBytes));

    // Look for a TSV key starting with the literal field bytes
    // (including the leading ※). That key MUST exist post-extractor-
    // fix, otherwise the engine would still miss at runtime.
    bool any_matched = false;
    for (const auto& kv : map) {
        if (kv.first.size() < field_prefix.size()) continue;
        if (kv.first.compare(0, field_prefix.size(), field_prefix) != 0) continue;
        // Literal lookup must hit — the un-stripped form is the TSV key.
        const std::string* en = LookupWithStrippedPrefix(map, kv.first);
        EXPECT_NE(en, nullptr)
            << "literal lookup failed for key starting with the field "
               "MISS bytes 0x81 0xA6 0x92 0x6A 0x90 0x53 ...";
        any_matched = true;
        break;
    }
    EXPECT_TRUE(any_matched)
        << "no TSV key starts with ※男心を鷲… — extractor regressed back "
        "to the over-aggressive strip OR translations.tsv is stale.";
}

// REGRESSION (literal byte-path): the user-reported "Ａ定食は三時間目
// には売り切れるから…" line. OLD pipeline produced a TSV key without
// the leading Ａ; engine sent the un-stripped form and missed.  Post-
// extractor-fix the TSV holds the un-stripped form, so the engine
// bytes match LITERALLY. Bytes below are the CP932 encoding of the
// full-width "Ａ定食は三時間目" — engine emits 0x82 0x60 (Ａ), not
// ASCII 'A'. If this changes in the source data, update accordingly.
TEST_F(TranslationPipelineTest, FieldReportedSetMealLineResolvesLiterally) {
    static const unsigned char kFieldBytes[] = {
        0x82, 0x60,  // Ａ (full-width)
        0x92, 0xE8,  // 定
        0x90, 0x48,  // 食
        0x82, 0xCD,  // は
        0x8E, 0x4F,  // 三
        0x8E, 0x9E,  // 時
        0x8A, 0xD4,  // 間
        0x96, 0xDA,  // 目
    };
    std::string field_prefix(reinterpret_cast<const char*>(kFieldBytes),
                              sizeof(kFieldBytes));

    bool any_matched = false;
    for (const auto& kv : map) {
        if (kv.first.size() < field_prefix.size()) continue;
        if (kv.first.compare(0, field_prefix.size(), field_prefix) != 0) continue;
        const std::string* en = LookupWithStrippedPrefix(map, kv.first);
        EXPECT_NE(en, nullptr)
            << "literal lookup failed for key starting with the field "
               "bytes 0x82 0x60 0x92 0xE8 0x90 0x48 ... (Ａ定食は…).";
        any_matched = true;
        break;
    }
    EXPECT_TRUE(any_matched)
        << "no TSV key starts with Ａ定食は三時間目 — extractor regressed "
        "back to the over-aggressive strip OR translations.tsv is stale.";
}

// REGRESSION (defense-in-depth): the runtime fallback strip. With the
// extractor fixed, the field-bug bytes hit literally. But if a NEW
// game / future regression resurfaces a TSV with leading-decorator
// keys stripped, the runtime fallback should still recover them.
// Synthetic test against a tiny in-memory map (the real TSV is now
// un-stripped, so prepending ※ to its keys would over-strip past the
// leading char — covered by UTs instead).
TEST_F(TranslationPipelineTest, FallbackStillStripsLeadingDecorator) {
    TranslationMap synthetic{ {"\x92\xE8\x90\x48", "set meal"} };  // 定食
    const std::string* en = LookupWithStrippedPrefix(
        synthetic, std::string("\x81\xA6\x92\xE8\x90\x48", 6));    // ※定食
    ASSERT_NE(en, nullptr) << "fallback strip is broken — UT regression";
    EXPECT_EQ(*en, "set meal");
}

// REGRESSION: literal-key lookup must always succeed too — the fallback
// shouldn't accidentally break the forward path.
TEST_F(TranslationPipelineTest, EveryJpResolvesViaForwardLookup) {
    int violations = 0;
    std::string first_violation;
    for (const auto& kv : map) {
        const std::string* en = LookupWithStrippedPrefix(map, kv.first);
        if (en == nullptr || *en != kv.second) {
            ++violations;
            if (first_violation.empty()) first_violation = kv.first;
        }
    }
    EXPECT_EQ(violations, 0)
        << violations << " literal lookups failed (first: '"
        << first_violation << "')";
}

// REGRESSION: a 10..14-byte choice-like JP that's a SUBSTRING of a
// longer dialogue JP got the longer line spliced with the choice's
// truncated EN by the FORCED pass / is_choice_like override. With both
// removed, the patcher's main pass requires `validStart` && `validEnd`
// (both bytes adjacent to the match must be null). This test wires up
// real (short, long) substring pairs from the TSV — if any exist where
// short is choice-like and long contains short — runs the patcher
// against a buffer holding the longer line, and asserts the long bytes
// are intact.
TEST_F(TranslationPipelineTest, ChoiceJpInsideLongerJpDoesNotCorruptLonger) {
    // Build a sorted list of long entries (JP > 14 bytes) and walk a
    // sample of choice-like entries against them. We cap work at a few
    // thousand pair-tests so this stays well under a second.
    std::vector<const std::string*> longer;
    longer.reserve(map.size());
    for (const auto& kv : map) {
        if (kv.first.size() > 14) longer.push_back(&kv.first);
    }

    int pair_tests   = 0;
    int violations   = 0;
    std::pair<std::string, std::string> first_violation;
    constexpr int kMaxPairs = 4000;

    for (const auto& kv : map) {
        if (!IsChoiceLikeJp(kv.first)) continue;
        const std::string& sjp = kv.first;
        for (const std::string* ljp : longer) {
            if (pair_tests >= kMaxPairs) break;
            // find sjp inside ljp; skip when ljp == sjp (shouldn't happen
            // given length filter, but defensive).
            if (ljp->find(sjp) == std::string::npos) continue;
            ++pair_tests;

            // Buffer: [\0][long_jp\0]. Pass ONLY the short entry to the
            // patcher — we're testing whether the short JP's substring
            // match inside long JP corrupts the long bytes. Including
            // the long entry too would have the patcher legitimately
            // write the long EN into its slot (when it fits), which
            // changes the buffer for non-regression reasons.
            std::vector<unsigned char> buf;
            buf.push_back(0);
            buf.insert(buf.end(), ljp->begin(), ljp->end());
            buf.push_back(0);

            TranslationMap only_short{ { sjp, map.at(sjp) } };
            auto stats = PatchTranslationsInPlace(buf.data(),
                                                   buf.data() + buf.size(),
                                                   only_short);

            if (stats.hits != 0 ||
                std::memcmp(buf.data() + 1, ljp->data(), ljp->size()) != 0) {
                ++violations;
                if (first_violation.first.empty()) {
                    first_violation = { sjp, *ljp };
                }
            }
        }
        if (pair_tests >= kMaxPairs) break;
    }
    EXPECT_EQ(violations, 0)
        << violations << " substring pairs caused longer-JP corruption "
        "(first: short='" << first_violation.first
        << "', long='" << first_violation.second << "')";
    // Sanity: the test is meaningful only if at least some pairs exist
    // in the data. If pair_tests == 0 the regression couldn't fire and
    // this test isn't exercising anything — flag it.
    EXPECT_GT(pair_tests, 0)
        << "no (choice-like JP, longer JP containing it) pairs found in "
        "the TSV — test is no-op for this dataset";
}
