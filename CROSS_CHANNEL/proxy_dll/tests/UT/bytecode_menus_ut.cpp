// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// UT — sn.bin bytecode menu/choice/backlog logic & op 0x86 signature contracts:
//   * translator_logic::SeedScriptMenuTranslations
//   * translator_logic::BuildChoiceBuffer
//   * translator_logic::FormatBacklogSlotText
//   * bytecode_hooks::ValidateOp86HandlerBytes

#include "bytecode_hooks.h"
#include "translator_logic.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

using translator_logic::BuildChoiceBuffer;
using translator_logic::FormatBacklogSlotText;
using translator_logic::LookupWithStrippedPrefix;
using translator_logic::SeedScriptMenuTranslations;
using translator_logic::TranslationMap;
using translator_logic::Utf8ToCp932;

struct RawChoiceOption {
    std::array<unsigned char, 10> preamble;
    std::string text;
};

std::vector<unsigned char> EncodeChoiceRecord(
    std::uint16_t count,
    std::uint16_t default_idx,
    const std::vector<RawChoiceOption>& options,
    bool terminate_last = true) {
    std::vector<unsigned char> buf;
    buf.push_back(0x80u);
    buf.push_back(0x80u);
    buf.push_back(static_cast<unsigned char>(count & 0xFFu));
    buf.push_back(static_cast<unsigned char>((count >> 8) & 0xFFu));
    buf.push_back(static_cast<unsigned char>(default_idx & 0xFFu));
    buf.push_back(static_cast<unsigned char>((default_idx >> 8) & 0xFFu));

    for (std::size_t i = 0; i < options.size(); ++i) {
        buf.insert(buf.end(), options[i].preamble.begin(), options[i].preamble.end());
        buf.insert(buf.end(), options[i].text.begin(), options[i].text.end());
        if (terminate_last || i + 1 < options.size()) {
            buf.push_back(0x00u);
        }
    }
    return buf;
}

struct ParsedChoiceRecord {
    std::array<unsigned char, 6> header{};
    std::vector<RawChoiceOption> options;
};

ParsedChoiceRecord DecodeChoiceRecord(const unsigned char* buf, std::size_t max_bytes) {
    ParsedChoiceRecord rec;
    std::memcpy(rec.header.data(), buf, 6);
    const std::uint16_t count =
        static_cast<std::uint16_t>(buf[2]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(buf[3]) << 8);
    std::size_t off = 6;
    for (std::uint16_t i = 0; i < count; ++i) {
        EXPECT_LE(off + 11, max_bytes);
        RawChoiceOption opt{};
        std::memcpy(opt.preamble.data(), buf + off, 10);
        off += 10;
        const std::size_t start = off;
        while (off < max_bytes && buf[off] != 0) {
            ++off;
        }
        EXPECT_LT(off, max_bytes);
        opt.text.assign(reinterpret_cast<const char*>(buf + start), off - start);
        ++off;
        rec.options.push_back(std::move(opt));
    }
    return rec;
}

bool TestPrefixLookup(const std::string& maybe_truncated, std::string& out_full) {
    if (maybe_truncated == "Check out the") {
        out_full = "Check out the shrine on the hill";
        return true;
    }
    return false;
}

// Exact 138-byte machine code of FUN_0083efb0 (op 0x86, RVA 0x2EFB0) at
// preferred ImageBase = 0x00810000 in unpacked cc_dumped.exe.
std::array<std::uint8_t, 0x8A> MakeCanonicalOp86Bytes() {
    return {{
        0x53, 0x8B, 0x1D, 0x70, 0xA8, 0xC2, 0x06, 0x56,
        0x8B, 0xF3, 0x57, 0x69, 0xF6, 0x84, 0x05, 0x00,
        0x00, 0xE8, 0x5A, 0x49, 0xFF, 0xFF, 0x98, 0x89,
        0x86, 0x80, 0xB8, 0xC2, 0x06, 0xE8, 0x4E, 0x49,
        0xFF, 0xFF, 0x8B, 0x3D, 0x78, 0x2B, 0xD9, 0x06,
        0x0F, 0xBF, 0xC8, 0x8D, 0x96, 0x88, 0xB8, 0xC2,
        0x06, 0x89, 0x8E, 0x84, 0xB8, 0xC2, 0x06, 0x8B,
        0xC7, 0x2B, 0xD7, 0xEB, 0x03, 0x8D, 0x49, 0x00,
        0x8A, 0x08, 0x88, 0x0C, 0x02, 0x40, 0x84, 0xC9,
        0x75, 0xF6, 0x8D, 0x9B, 0x00, 0x00, 0x00, 0x00,
        0x8A, 0x17, 0x47, 0x84, 0xD2, 0x75, 0xF9, 0x89,
        0x3D, 0x78, 0x2B, 0xD9, 0x06, 0x8D, 0xBE, 0x80,
        0xBA, 0xC2, 0x06, 0x43, 0x81, 0xE3, 0xFF, 0x03,
        0x00, 0x80, 0xB9, 0xE1, 0x00, 0x00, 0x00, 0xBE,
        0x90, 0x5E, 0xBE, 0x06, 0xF3, 0xA5, 0x79, 0x08,
        0x4B, 0x81, 0xCB, 0x00, 0xFC, 0xFF, 0xFF, 0x43,
        0x5F, 0x5E, 0x89, 0x1D, 0x70, 0xA8, 0xC2, 0x06,
        0x5B, 0xC3
    }};
}

void AddRelocDelta(std::uint8_t* p, std::uint32_t delta) {
    std::uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    v += delta;
    std::memcpy(p, &v, sizeof(v));
}

}  // namespace

// ---------------------------------------------------------------------------
// SeedScriptMenuTranslations contracts
// ---------------------------------------------------------------------------

TEST(SeedScriptMenuTranslations, IdempotentInsertPreservesExistingTsvAndWinsOverStrippedFallback) {
    TranslationMap fresh;
    const int full_inserted = SeedScriptMenuTranslations(fresh);
    ASSERT_EQ(fresh.size(), static_cast<std::size_t>(full_inserted));
    EXPECT_EQ(SeedScriptMenuTranslations(fresh), 0);
    EXPECT_EQ(fresh.size(), static_cast<std::size_t>(full_inserted));

    // Pre-existing TSV entries must never be overwritten, while unstripped
    // seeded keys ("FLOWER'S、解散の危機である。") take priority over
    // LookupWithStrippedPrefix()'s stripped fallback ("解散の危機である。").
    const std::string main_story_key = Utf8ToCp932("本編スタート");
    const std::string stripped_flowers_key = Utf8ToCp932("解散の危機である。");
    const std::string full_flowers_key = Utf8ToCp932("FLOWER'S、解散の危機である。");

    TranslationMap with_tsv{
        {main_story_key, "Custom TSV Override"},
        {stripped_flowers_key, "It's a breakup crisis."},
    };
    const int delta_inserted = SeedScriptMenuTranslations(with_tsv);
    EXPECT_EQ(delta_inserted, full_inserted - 1);
    EXPECT_EQ(SeedScriptMenuTranslations(with_tsv), 0);

    const std::string* override_hit = LookupWithStrippedPrefix(with_tsv, main_story_key);
    ASSERT_NE(override_hit, nullptr);
    EXPECT_EQ(*override_hit, "Custom TSV Override");

    const std::string* full_hit = LookupWithStrippedPrefix(with_tsv, full_flowers_key);
    ASSERT_NE(full_hit, nullptr);
    EXPECT_EQ(*full_hit, "FLOWER'S is facing a breakup crisis.");

    const std::string* stripped_hit = LookupWithStrippedPrefix(with_tsv, stripped_flowers_key);
    ASSERT_NE(stripped_hit, nullptr);
    EXPECT_EQ(*stripped_hit, "It's a breakup crisis.");
}

// ---------------------------------------------------------------------------
// BuildChoiceBuffer contracts
// ---------------------------------------------------------------------------

TEST(BuildChoiceBuffer, TranslatesScript0MenuPreservingHeaderAndOptionMetadata) {
    TranslationMap m;
    SeedScriptMenuTranslations(m);

    const std::vector<RawChoiceOption> src_opts = {
        {{{0x00, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0x4E, 0x04, 0x00, 0x00}},
         Utf8ToCp932("本編スタート")},
        {{{0x01, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0xC0, 0x05, 0x00, 0x00}},
         Utf8ToCp932("ファイル別実行")},
        {{{0x02, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0x50, 0x59, 0x01, 0x00}},
         Utf8ToCp932("アナザーを始める")},
        {{{0x03, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0xA8, 0x04, 0x00, 0x00}},
         Utf8ToCp932("ＸＢＯＸシナリオを始める")},
        {{{0x04, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0x76, 0x74, 0x01, 0x00}},
         Utf8ToCp932("その他シナリオ／実験場")},
    };

    const std::vector<unsigned char> in_buf = EncodeChoiceRecord(5, 0, src_opts);
    std::vector<unsigned char> out_buf(1024, 0xCCu);

    const std::size_t consumed = BuildChoiceBuffer(
        in_buf.data(), in_buf.size(), out_buf.data(), out_buf.size(), m);
    ASSERT_EQ(consumed, in_buf.size());

    const ParsedChoiceRecord parsed = DecodeChoiceRecord(out_buf.data(), out_buf.size());
    ASSERT_EQ(parsed.options.size(), 5u);
    EXPECT_EQ(std::memcmp(parsed.header.data(), in_buf.data(), 6), 0);
    for (std::size_t i = 0; i < src_opts.size(); ++i) {
        EXPECT_EQ(parsed.options[i].preamble, src_opts[i].preamble);
    }
    EXPECT_EQ(parsed.options[0].text, "Start Main Story");
    EXPECT_EQ(parsed.options[1].text, "Run by Script File");
    EXPECT_EQ(parsed.options[2].text, "Start Another Story");
    EXPECT_EQ(parsed.options[3].text, "Start Xbox Scenario");
    EXPECT_EQ(parsed.options[4].text, "Other Scenarios / Test Room");
}

TEST(BuildChoiceBuffer, ScansSourceOptionsBeyond64BytesUpTo512Bytes_Regression) {
    const std::string opt_511(511, 'A');
    const std::string opt_512(512, 'B');
    TranslationMap m{
        {opt_511, "Translated 511B option"},
        {opt_512, "Translated 512B option"},
    };

    const std::vector<RawChoiceOption> valid_opts = {
        {{{0x00, 0x80, 0x00, 0x00, 0xFF, 0xFF, 0x10, 0x00, 0x00, 0x00}}, opt_511},
        {{{0x01, 0x80, 0x01, 0x00, 0xFF, 0xFF, 0x20, 0x00, 0x00, 0x00}}, opt_512},
    };
    const std::vector<unsigned char> in_valid = EncodeChoiceRecord(2, 1, valid_opts);
    std::vector<unsigned char> out_buf(2048, 0);

    const std::size_t consumed = BuildChoiceBuffer(
        in_valid.data(), in_valid.size(), out_buf.data(), out_buf.size(), m);
    ASSERT_EQ(consumed, in_valid.size());

    const ParsedChoiceRecord parsed = DecodeChoiceRecord(out_buf.data(), out_buf.size());
    ASSERT_EQ(parsed.options.size(), 2u);
    EXPECT_EQ(parsed.options[0].text, "Translated 511B option");
    EXPECT_EQ(parsed.options[1].text, "Translated 512B option");

    const std::vector<RawChoiceOption> too_long_opts = {
        {{{0x00, 0x80, 0x00, 0x00, 0xFF, 0xFF, 0x10, 0x00, 0x00, 0x00}},
         std::string(513, 'C')},
    };
    const std::vector<unsigned char> in_513 = EncodeChoiceRecord(1, 0, too_long_opts);
    EXPECT_EQ(BuildChoiceBuffer(in_513.data(), in_513.size(),
                                out_buf.data(), out_buf.size(), m),
              0u);
}

TEST(BuildChoiceBuffer, ClampsOverlongTranslatedOptionAtWordBoundaryAndHard255Limit) {
    // 50 repetitions of "alpha " = 300 bytes. Within the 255-byte payload cap
    // (kMaxChoiceOptionOutBytes - 1), 42 words + 41 spaces = 251 bytes and byte
    // 251 is a space, so FitToSlot cuts cleanly to 42 "alpha" words (251 bytes).
    std::string spaced_300;
    for (int i = 0; i < 50; ++i) {
        spaced_300 += "alpha ";
    }
    std::string expected_42_words;
    for (int i = 0; i < 42; ++i) {
        if (i > 0) {
            expected_42_words.push_back(' ');
        }
        expected_42_words += "alpha";
    }
    ASSERT_EQ(expected_42_words.size(), 251u);

    const std::string jp_worded = Utf8ToCp932("しゅーるすとれみんぐパンに挑戦する");
    const std::string jp_spaceless = Utf8ToCp932("選択肢ハードクリップ");
    TranslationMap m{
        {jp_worded, spaced_300},
        {jp_spaceless, std::string(300, 'X')},
    };

    const std::vector<RawChoiceOption> opts = {
        {{{0x00, 0x80, 0x00, 0x00, 0xFF, 0xFF, 0x69, 0x3E, 0x00, 0x00}}, jp_worded},
        {{{0x01, 0x80, 0x01, 0x00, 0xFF, 0xFF, 0x70, 0x3E, 0x00, 0x00}}, jp_spaceless},
    };
    const std::vector<unsigned char> in_buf = EncodeChoiceRecord(2, 0, opts);
    std::vector<unsigned char> out_buf(1024, 0);

    const std::size_t consumed = BuildChoiceBuffer(
        in_buf.data(), out_buf.data(), out_buf.size(), m);
    ASSERT_EQ(consumed, in_buf.size());

    const ParsedChoiceRecord parsed = DecodeChoiceRecord(out_buf.data(), out_buf.size());
    ASSERT_EQ(parsed.options.size(), 2u);
    EXPECT_EQ(parsed.options[0].text, expected_42_words);
    EXPECT_EQ(parsed.options[1].text, std::string(255, 'X'));
}

TEST(BuildChoiceBuffer, ResolvesStrippedPrefixSpacePaddedAndTruncatedPrefixOptions) {
    const std::string stripped_jp = Utf8ToCp932("選択肢テスト");
    const std::string decorated_jp = Utf8ToCp932("※選択肢テスト");
    const std::string unmapped_jp_padded = Utf8ToCp932("未翻訳選択肢") + "   ";
    TranslationMap m{ {stripped_jp, "Decorated Choice Hit"} };

    const std::vector<RawChoiceOption> opts = {
        {{{0x00, 0x80, 0, 0, 0xFF, 0xFF, 1, 0, 0, 0}}, decorated_jp},
        {{{0x01, 0x80, 1, 0, 0xFF, 0xFF, 2, 0, 0, 0}}, "Check out the   "},
        {{{0x02, 0x80, 2, 0, 0xFF, 0xFF, 3, 0, 0, 0}}, "Already Clean EN    "},
        {{{0x03, 0x80, 3, 0, 0xFF, 0xFF, 4, 0, 0, 0}}, unmapped_jp_padded},
    };
    const std::vector<unsigned char> in_buf = EncodeChoiceRecord(4, 0, opts);
    std::vector<unsigned char> out_buf(1024, 0);

    const std::size_t consumed = BuildChoiceBuffer(
        in_buf.data(), in_buf.size(), out_buf.data(), out_buf.size(),
        m, &TestPrefixLookup);
    ASSERT_EQ(consumed, in_buf.size());

    const ParsedChoiceRecord parsed = DecodeChoiceRecord(out_buf.data(), out_buf.size());
    ASSERT_EQ(parsed.options.size(), 4u);
    EXPECT_EQ(parsed.options[0].text, "Decorated Choice Hit");
    EXPECT_EQ(parsed.options[1].text, "Check out the shrine on the hill");
    EXPECT_EQ(parsed.options[2].text, "Already Clean EN");
    // Unmapped Japanese bytes retain their original CP932 payload verbatim.
    EXPECT_EQ(parsed.options[3].text, unmapped_jp_padded);
}

TEST(BuildChoiceBuffer, RejectsMalformedCountsUnterminatedInputAndSmallOutCapacity) {
    TranslationMap m;
    const std::vector<RawChoiceOption> one_opt = {
        {{{0x00, 0x80, 0, 0, 0xFF, 0xFF, 1, 0, 0, 0}}, "CCA0001"},
    };
    const std::vector<unsigned char> valid = EncodeChoiceRecord(1, 0, one_opt);
    std::vector<unsigned char> out_buf(256, 0);

    EXPECT_EQ(BuildChoiceBuffer(nullptr, valid.size(), out_buf.data(), out_buf.size(), m), 0u);
    EXPECT_EQ(BuildChoiceBuffer(valid.data(), valid.size(), nullptr, out_buf.size(), m), 0u);
    EXPECT_EQ(BuildChoiceBuffer(valid.data(), 5, out_buf.data(), out_buf.size(), m), 0u);

    const std::vector<unsigned char> zero_cnt = EncodeChoiceRecord(0, 0, {});
    EXPECT_EQ(BuildChoiceBuffer(zero_cnt.data(), zero_cnt.size(),
                                out_buf.data(), out_buf.size(), m), 0u);

    const std::vector<unsigned char> seventeen_cnt = EncodeChoiceRecord(17, 0, one_opt);
    EXPECT_EQ(BuildChoiceBuffer(seventeen_cnt.data(), seventeen_cnt.size(),
                                out_buf.data(), out_buf.size(), m), 0u);

    const std::vector<unsigned char> unterminated = EncodeChoiceRecord(1, 0, one_opt, false);
    EXPECT_EQ(BuildChoiceBuffer(unterminated.data(), unterminated.size(),
                                out_buf.data(), out_buf.size(), m), 0u);

    // Needed output: 6 header + 10 preamble + 7 chars + 1 NUL = 24 bytes.
    EXPECT_EQ(BuildChoiceBuffer(valid.data(), valid.size(), out_buf.data(), 23, m), 0u);
    EXPECT_EQ(BuildChoiceBuffer(valid.data(), valid.size(), out_buf.data(), 24, m), valid.size());
}

// ---------------------------------------------------------------------------
// FormatBacklogSlotText & op 0x86 contracts
// ---------------------------------------------------------------------------

TEST(FormatBacklogSlotText, WrapsAtColumnSeamAndNormalizesParagraphDelimiters) {
    const std::string k9  = Utf8ToCp932("行９");
    const std::string k10 = Utf8ToCp932("行１０");
    const std::string k11 = Utf8ToCp932("行１１");
    const std::string kPara = Utf8ToCp932("段落");
    const std::string kLongWord = Utf8ToCp932("長語");

    TranslationMap m{
        {k9, "abcd efgh"},
        {k10, "abcd efghi"},
        {k11, "abcd efghij"},
        {kPara, "One two\r\nThree%NFour five six"},
        {kLongWord, "ABCDEFGHIJKLMNO"},
    };

    std::string out;
    ASSERT_TRUE(FormatBacklogSlotText(k9, m, out, 10u, 64u));
    EXPECT_EQ(out, "abcd efgh");

    ASSERT_TRUE(FormatBacklogSlotText(k10, m, out, 10u, 64u));
    EXPECT_EQ(out, "abcd efghi");

    ASSERT_TRUE(FormatBacklogSlotText(k11, m, out, 10u, 64u));
    EXPECT_EQ(out, "abcd%Nefghij");

    ASSERT_TRUE(FormatBacklogSlotText(kPara, m, out, 10u, 64u));
    EXPECT_EQ(out, "One two%NThree%NFour five%Nsix");

    ASSERT_TRUE(FormatBacklogSlotText(kLongWord, m, out, 6u, 64u));
    EXPECT_EQ(out, "ABCDEF%NGHIJKL%NMNO");
}

TEST(FormatBacklogSlotText, ClampsAtSlotCapacityWithoutDanglingPercentAndLeavesOutputUntouchedOnError) {
    const std::string key = Utf8ToCp932("「先輩……っ！」");
    // Paragraph 2 ("beta gammas" = 11 chars) wraps at wrap_cols = 10 ->
    // "alpha%Nbeta%Ngammas" (19 bytes):
    //   [0..4] = "alpha", [5..6] = "%N", [7..10] = "beta", [11..12] = "%N", [13..18] = "gammas".
    TranslationMap m{ {key, "alpha%Nbeta gammas"} };

    std::string out = "initial";
    // slot_capacity = 20 (max_payload = 19) fits the entire 19-byte wrapped string;
    // slot_capacity = 14 (max_payload = 13) cuts at "alpha%Nbeta%N" -> strips "%N" -> "alpha%Nbeta".
    ASSERT_TRUE(FormatBacklogSlotText(key, m, out, 10u, 20u));
    EXPECT_EQ(out, "alpha%Nbeta%Ngammas");
    ASSERT_TRUE(FormatBacklogSlotText(key, m, out, 10u, 14u));
    EXPECT_EQ(out, "alpha%Nbeta");

    // slot_capacity = 7 (max_payload = 6) cuts mid-token at "alpha%", which
    // must strip the dangling '%' rather than leaving broken control syntax.
    ASSERT_TRUE(FormatBacklogSlotText(key, m, out, 10u, 7u));
    EXPECT_EQ(out, "alpha");

    // slot_capacity = 8 (max_payload = 7) cuts right after "alpha%N", which
    // must strip the trailing empty "%N" line break.
    ASSERT_TRUE(FormatBacklogSlotText(key, m, out, 10u, 8u));
    EXPECT_EQ(out, "alpha");

    // Error paths (empty key, unknown key, slot_capacity < 2) return false and
    // preserve the caller's existing output buffer untouched.
    std::string untouched = "keep-me";
    EXPECT_FALSE(FormatBacklogSlotText("", m, untouched, 10u, 64u));
    EXPECT_EQ(untouched, "keep-me");
    EXPECT_FALSE(FormatBacklogSlotText("UNKNOWN_KEY", m, untouched, 10u, 64u));
    EXPECT_EQ(untouched, "keep-me");
    EXPECT_FALSE(FormatBacklogSlotText(key, m, untouched, 10u, 1u));
    EXPECT_EQ(untouched, "keep-me");
}

TEST(Op86SignatureValidation, ValidatesPreferredBaseAslrRebaseAndRejectsCorruptedInputs) {
    const auto canonical = MakeCanonicalOp86Bytes();
    std::uint32_t global_va = 0;
    ASSERT_TRUE(bytecode_hooks::ValidateOp86HandlerBytes(
        canonical.data(), canonical.size(), &global_va));
    EXPECT_EQ(global_va, bytecode_hooks::kPreferredDialogueTextPtrVa);
    EXPECT_TRUE(bytecode_hooks::ValidateOp86HandlerBytes(
        canonical.data(), canonical.size(), nullptr));

    // Simulate ASLR load at +0x01350000 delta from preferred 0x00810000.
    auto rebased = canonical;
    constexpr std::uint32_t kAslrDelta = 0x01350000u;
    for (std::size_t reloc_off : {0x03u, 0x19u, 0x24u, 0x2Du, 0x33u,
                                  0x59u, 0x5Fu, 0x70u, 0x84u}) {
        AddRelocDelta(rebased.data() + reloc_off, kAslrDelta);
    }

    std::uint32_t rebased_global_va = 0;
    ASSERT_TRUE(bytecode_hooks::ValidateOp86HandlerBytes(
        rebased.data(), rebased.size(), &rebased_global_va));
    EXPECT_EQ(rebased_global_va,
              bytecode_hooks::kPreferredDialogueTextPtrVa + kAslrDelta);

    // Null pointer and avail_bytes = kOp86HandlerMinBytes - 1 (0x89) must reject
    // without mutating out_text_ptr_global_va.
    std::uint32_t sentinel_va = 0xDEADBEEFu;
    EXPECT_FALSE(bytecode_hooks::ValidateOp86HandlerBytes(
        nullptr, canonical.size(), &sentinel_va));
    EXPECT_FALSE(bytecode_hooks::ValidateOp86HandlerBytes(
        canonical.data(), canonical.size() - 1u, &sentinel_va));
    EXPECT_EQ(sentinel_va, 0xDEADBEEFu);

    // Zero global VA (0x00000000) in both pre and post sites must reject.
    auto zero_global = canonical;
    constexpr std::uint32_t kZero = 0u;
    std::memcpy(zero_global.data() + 0x24u, &kZero, sizeof(kZero));
    std::memcpy(zero_global.data() + 0x59u, &kZero, sizeof(kZero));
    EXPECT_FALSE(bytecode_hooks::ValidateOp86HandlerBytes(
        zero_global.data(), zero_global.size(), &sentinel_va));

    // Corrupting post-hook global operand so pre != post must reject.
    auto mismatched = rebased;
    AddRelocDelta(mismatched.data() + 0x59u, 4u);
    EXPECT_FALSE(bytecode_hooks::ValidateOp86HandlerBytes(
        mismatched.data(), mismatched.size(), &sentinel_va));

    // Corrupted prologue (`push ebx; mov ebx, [...]`) or anchor (`imul esi, esi, 0x584`) must reject.
    auto bad_prologue = canonical;
    bad_prologue[0] ^= 0xFFu;
    EXPECT_FALSE(bytecode_hooks::ValidateOp86HandlerBytes(
        bad_prologue.data(), bad_prologue.size(), &sentinel_va));

    auto bad_anchor = canonical;
    bad_anchor[0x0Cu] ^= 0xFFu;
    EXPECT_FALSE(bytecode_hooks::ValidateOp86HandlerBytes(
        bad_anchor.data(), bad_anchor.size(), &sentinel_va));
    EXPECT_EQ(sentinel_va, 0xDEADBEEFu);
}
