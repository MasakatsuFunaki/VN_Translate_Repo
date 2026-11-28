// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Pure unit tests for translator_logic (BOM detection, CP932-safe TSV
// unescaping/parsing, runtime lookup, UI font sizing, FXF title extraction)
// and patcher_logic (x86 rel32 branch emission, prologue validation, ASCII
// and byte-patch helpers).
#include "patcher_logic.h"
#include "translator_logic.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace pl = patcher_logic;

namespace {

constexpr UINT kCp932CodePage = 932;
constexpr int kChoiceOrigFontSize = 24;
constexpr int kChoiceOrigSpace = 1;
constexpr int kChoiceAnimMaxChars = 12;
constexpr std::uint8_t kFxfXorMask = 0xFFu;
constexpr std::uintptr_t kRelBaseNextIp = 0x00401005u;
constexpr std::uint32_t kFwdRelDelta = 0x00123456u;
constexpr std::uint32_t kBackRelDelta = 0x00000010u;

std::string ToCp932(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    BOOL used_default = FALSE;
    const int needed = WideCharToMultiByte(
        kCp932CodePage, WC_NO_BEST_FIT_CHARS, wide.data(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, &used_default);
    if (needed <= 0 || used_default) {
        throw std::runtime_error("Wide string cannot be represented in CP932");
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    const int written = WideCharToMultiByte(
        kCp932CodePage, WC_NO_BEST_FIT_CHARS, wide.data(),
        static_cast<int>(wide.size()), out.data(), needed, nullptr, &used_default);
    if (written != needed || used_default) {
        throw std::runtime_error("CP932 conversion failed");
    }
    return out;
}

}  // namespace

TEST(TranslatorContract, BomUnescapeAndParseTsvPreserveCp932BackslashTrailBytes) {
    // UTF-8 BOM boundary matrix.
    const std::string bom_only("\xEF\xBB\xBF", 3);
    const std::string bom_plus = bom_only + "abc";
    const std::string two_bytes("\xEF\xBB", 2);
    const std::string non_bom("\xEF\xBB\xBE", 3);
    EXPECT_EQ(translator_logic::Utf8BomLen(nullptr, 3), 0u);
    EXPECT_EQ(translator_logic::Utf8BomLen(bom_only.data(), 0), 0u);
    EXPECT_EQ(translator_logic::Utf8BomLen(two_bytes.data(), two_bytes.size()), 0u);
    EXPECT_EQ(translator_logic::Utf8BomLen(non_bom.data(), non_bom.size()), 0u);
    EXPECT_EQ(translator_logic::Utf8BomLen(bom_only.data(), bom_only.size()), 3u);
    EXPECT_EQ(translator_logic::Utf8BomLen(bom_plus.data(), bom_plus.size()), 3u);

    // Verify CP932 '―' genuinely encodes with lead 0x81 and trail byte 0x5C ('\\').
    ASSERT_EQ(ToCp932(L"―"), std::string("\x81\\", 2));

    // Boundary matrix covering both CP932 lead-byte ranges ([0x81..0x9F], [0xE0..0xFC])
    // and every non-lead high-bit gap (0x80, [0xA0..0xDF] half-width katakana, 0xFD).
    struct UnescapeCase {
        std::string escaped;
        std::string expected;
        const char* label;
    };
    const std::vector<UnescapeCase> unescape_cases = {
        {std::string("\x80\\r\\n", 5),
         std::string("\x80\r\n", 3),
         "non-lead-0x80-below-range1-unescapes"},
        {ToCp932(L"そして――――") + "\\r\\n",
         ToCp932(L"そして――――\r\n"),
         "lead-range1-min-0x81-horizontal-bar-before-crlf"},
        {ToCp932(L"―") + "\\n" + ToCp932(L"―") + "\\t" + ToCp932(L"―") + "\\\\",
         ToCp932(L"―\n―\t―\\"),
         "lead-range1-min-0x81-before-n-t-backslash"},
        {std::string("\x9F\\r", 3),
         std::string("\x9F\\r", 3),
         "lead-range1-max-0x9f-preserves-0x5c-trail"},
        {std::string("\xA0\\r", 3),
         std::string("\xA0\r", 2),
         "non-lead-0xa0-above-range1-unescapes"},
        {std::string("\xB1\\r\\n", 5),
         std::string("\xB1\r\n", 3),
         "non-lead-halfwidth-katakana-0xb1-unescapes"},
        {std::string("\xDF\\t", 3),
         std::string("\xDF\t", 2),
         "non-lead-0xdf-below-range2-unescapes"},
        {std::string("\xE0\\r", 3),
         std::string("\xE0\\r", 3),
         "lead-range2-min-0xe0-preserves-0x5c-trail"},
        {std::string("\xFC\\n", 3),
         std::string("\xFC\\n", 3),
         "lead-range2-max-0xfc-preserves-0x5c-trail"},
        {std::string("\xFD\\n", 3),
         std::string("\xFD\n", 2),
         "non-lead-0xfd-above-range2-unescapes"},
        {"a\\xb\\", "a\\xb\\", "unknown-and-trailing-backslash"},
        {std::string("\x81", 1),
         std::string("\x81", 1),
         "truncated-trailing-lead-byte"},
    };
    for (const auto& tc : unescape_cases) {
        SCOPED_TRACE(tc.label);
        std::string actual = tc.escaped;
        translator_logic::UnescapeInPlace(actual);
        EXPECT_EQ(actual, tc.expected);
    }

    // ParseTsvBuffer: null/empty rejection + mixed valid & malformed rows.
    translator_logic::TranslationMap empty_map;
    EXPECT_EQ(translator_logic::ParseTsvBuffer(nullptr, 10, empty_map), 0);
    EXPECT_EQ(translator_logic::ParseTsvBuffer("", 0, empty_map), 0);
    EXPECT_TRUE(empty_map.empty());

    const std::string tsv =
        "\n"
        "missing_tab_line\r\n"
        "\tempty_key\n"
        "empty_val\t\n" +
        ToCp932(L"夢美") + "\\r\\n" + ToCp932(L"「あ」") + "\tYumemi: \"Ah\"\\r\\n\r\n" +
        ToCp932(L"俺は、限界までスピードを上げて夢美を突き続け、") + "\\r\\n" +
        ToCp932(L"そして――――") + "\\r\\n" +
        "\tI pushed my speed to the limit, thrusting into Yumemi, and then--\\r\\n\n" +
        ToCp932(L"まるで犬のように小刻みに――――") + "\\r\\n" +
        "\tShort and rapid, just like a dog--\\r\\n\n" +
        ToCp932(L"夢美を呼ぶ") + "\tCall Yumemi\n" +
        ToCp932(L"アゲハを呼ぶ") + "\tCall Ageha";

    translator_logic::TranslationMap map;
    constexpr int kExpectedValidEntries = 5;
    ASSERT_EQ(translator_logic::ParseTsvBuffer(tsv.data(), tsv.size(), map),
              kExpectedValidEntries);
    EXPECT_EQ(map.size(), static_cast<std::size_t>(kExpectedValidEntries));
    EXPECT_EQ(map[ToCp932(L"夢美\r\n「あ」")], "Yumemi: \"Ah\"\r\n");
    EXPECT_EQ(map[ToCp932(L"俺は、限界までスピードを上げて夢美を突き続け、\r\nそして――――\r\n")],
              "I pushed my speed to the limit, thrusting into Yumemi, and then--\r\n");
    EXPECT_EQ(map[ToCp932(L"まるで犬のように小刻みに――――\r\n")],
              "Short and rapid, just like a dog--\r\n");
    EXPECT_EQ(map[ToCp932(L"夢美を呼ぶ")], "Call Yumemi");
    EXPECT_EQ(map[ToCp932(L"アゲハを呼ぶ")], "Call Ageha");
}

TEST(TranslatorContract, LookupFlattenUiFontAdjustmentAndFxfTitleBoundaries) {
    // FlattenNewlinesInPlace boundaries.
    {
        std::string s = "Line one\r\n\r\nLine two \r\n  ";
        translator_logic::FlattenNewlinesInPlace(s);
        EXPECT_EQ(s, "Line one Line two");
    }

    // LookupTranslation: %, color prefix, single-line flattening, misses/null.
    translator_logic::TranslationMap map;
    map[ToCp932(L"%#黄エタ")] = "%Use Eternal\r\nMode?\r\n";
    map[ToCp932(L"夢美を呼ぶ")] = "Call Yumemi";

    std::string out;
    EXPECT_FALSE(translator_logic::LookupTranslation(map, nullptr, false, out));
    EXPECT_FALSE(translator_logic::LookupTranslation(map, "", false, out));
    EXPECT_FALSE(translator_logic::LookupTranslation(map, "#", false, out));
    EXPECT_FALSE(translator_logic::LookupTranslation(map, "unmatched", false, out));

    const std::string runtime_prompt = ToCp932(L"#黄エタ");
    ASSERT_TRUE(translator_logic::LookupTranslation(map, runtime_prompt.c_str(), true, out));
    EXPECT_EQ(out, ToCp932(L"#黄Use Eternal Mode?"));

    const std::string choice_key = ToCp932(L"夢美を呼ぶ");
    ASSERT_TRUE(translator_logic::LookupTranslation(map, choice_key.c_str(), true, out));
    EXPECT_EQ(out, "Call Yumemi");

    // HasAsciiWords & VisibleAsciiLen boundaries (control prefix stripping, CP932 bailout).
    EXPECT_FALSE(translator_logic::HasAsciiWords(nullptr));
    EXPECT_FALSE(translator_logic::HasAsciiWords("A"));
    EXPECT_TRUE(translator_logic::HasAsciiWords("AB"));
    const std::string prefixed_ascii = ToCp932(L"%#黄Hi");
    EXPECT_TRUE(translator_logic::HasAsciiWords(prefixed_ascii.c_str()));
    EXPECT_EQ(translator_logic::VisibleAsciiLen(prefixed_ascii.c_str()), 2u);
    const std::string mixed_jp = std::string("AB") + ToCp932(L"あ");
    EXPECT_FALSE(translator_logic::HasAsciiWords(mixed_jp.c_str()));

    // ComputeUiFontAdjustment exact boundary seams.
    const std::string len_44(translator_logic::kMediumChoiceCharLimit, 'a');
    const std::string len_45(translator_logic::kMediumChoiceCharLimit + 1u, 'a');

    // curSpace <= 0 or curFontSize < kMinUiUnpatchedFontSize => no adjustment.
    EXPECT_FALSE(translator_logic::ComputeUiFontAdjustment(
                     len_44.c_str(), true, kChoiceOrigFontSize, 0, kChoiceAnimMaxChars)
                     .shouldAdjust);
    EXPECT_FALSE(translator_logic::ComputeUiFontAdjustment(
                     len_44.c_str(), true,
                     translator_logic::kMinUiUnpatchedFontSize - 1,
                     kChoiceOrigSpace, kChoiceAnimMaxChars)
                     .shouldAdjust);
    // Untranslated Japanese => no adjustment; untranslated ASCII words => adjusts.
    EXPECT_FALSE(translator_logic::ComputeUiFontAdjustment(
                     choice_key.c_str(), false, kChoiceOrigFontSize,
                     kChoiceOrigSpace, kChoiceAnimMaxChars)
                     .shouldAdjust);
    const auto adj_at_44 = translator_logic::ComputeUiFontAdjustment(
        len_44.c_str(), false, kChoiceOrigFontSize, kChoiceOrigSpace, kChoiceOrigFontSize);
    EXPECT_TRUE(adj_at_44.shouldAdjust);
    EXPECT_EQ(adj_at_44.newFontSize, translator_logic::kStandardChoiceFontSize);
    EXPECT_EQ(adj_at_44.newSpace, translator_logic::kPatchedChoiceSpace);
    EXPECT_EQ(adj_at_44.yOffset,
              (kChoiceOrigFontSize - translator_logic::kStandardChoiceFontSize) / 2);
    EXPECT_TRUE(adj_at_44.expandMaxChars);

    const auto adj_at_45 = translator_logic::ComputeUiFontAdjustment(
        len_45.c_str(), true, kChoiceOrigFontSize, kChoiceOrigSpace, kChoiceOrigFontSize + 1);
    EXPECT_TRUE(adj_at_45.shouldAdjust);
    EXPECT_EQ(adj_at_45.newFontSize, translator_logic::kCompactChoiceFontSize);
    EXPECT_FALSE(adj_at_45.expandMaxChars);

    // ExtractFxfWindowTitle: plaintext, XOR-encoded, prefix collision, empty/missing.
    const std::string fxf_plain =
        "windowTitleTaiken Ignored Trial\r\n"
        "  \twindowTitle\t  Welcome to AnzU! CE  \r\n";
    EXPECT_EQ(translator_logic::ExtractFxfWindowTitle(
                  reinterpret_cast<const std::uint8_t*>(fxf_plain.data()), fxf_plain.size()),
              "Welcome to AnzU! CE");

    std::vector<std::uint8_t> fxf_xor(fxf_plain.size());
    for (std::size_t i = 0; i < fxf_plain.size(); ++i) {
        fxf_xor[i] =
            static_cast<std::uint8_t>(static_cast<std::uint8_t>(fxf_plain[i]) ^ kFxfXorMask);
    }
    EXPECT_EQ(translator_logic::ExtractFxfWindowTitle(fxf_xor.data(), fxf_xor.size()),
              "Welcome to AnzU! CE");
    EXPECT_TRUE(translator_logic::ExtractFxfWindowTitle(nullptr, 0).empty());
    const std::string no_title = "name mainControl\r\nwindowTitle   \r\n";
    EXPECT_TRUE(translator_logic::ExtractFxfWindowTitle(
                    reinterpret_cast<const std::uint8_t*>(no_title.data()), no_title.size())
                    .empty());
}

TEST(PatcherLogicContract, EmitsExactRel32DisplacementsAndValidatesPrologueAndAsciiSeams) {
    // Rel32Offset forward & backward signed displacement.
    EXPECT_EQ(pl::Rel32Offset(kRelBaseNextIp, kRelBaseNextIp + kFwdRelDelta),
              static_cast<std::int32_t>(kFwdRelDelta));
    EXPECT_EQ(pl::Rel32Offset(kRelBaseNextIp, kRelBaseNextIp - kBackRelDelta),
              -static_cast<std::int32_t>(kBackRelDelta));

    // EmitRelCall32 forward: E8 + little-endian 0x00123456.
    std::array<std::uint8_t, pl::kRelBranchInstructionSize> call_buf{};
    const auto call_next_ip =
        reinterpret_cast<std::uintptr_t>(call_buf.data() + pl::kRelBranchInstructionSize);
    EXPECT_EQ(pl::EmitRelCall32(call_buf.data(), call_next_ip + kFwdRelDelta),
              call_buf.data() + pl::kRelBranchInstructionSize);
    const std::array<std::uint8_t, pl::kRelBranchInstructionSize> expected_call = {
        0xE8u, 0x56u, 0x34u, 0x12u, 0x00u};
    EXPECT_EQ(call_buf, expected_call);

    // EmitRelJmp32 backward (-16 = 0xFFFFFFF0): E9 F0 FF FF FF.
    std::array<std::uint8_t, pl::kRelBranchInstructionSize> jmp_buf{};
    const auto jmp_next_ip =
        reinterpret_cast<std::uintptr_t>(jmp_buf.data() + pl::kRelBranchInstructionSize);
    EXPECT_EQ(pl::EmitRelJmp32(jmp_buf.data(), jmp_next_ip - kBackRelDelta),
              jmp_buf.data() + pl::kRelBranchInstructionSize);
    const std::array<std::uint8_t, pl::kRelBranchInstructionSize> expected_jmp = {
        0xE9u, 0xF0u, 0xFFu, 0xFFu, 0xFFu};
    EXPECT_EQ(jmp_buf, expected_jmp);

    // FillNops, EmitBytes, BytesMatch.
    std::array<std::uint8_t, 4> patch_buf = {0x11u, 0x22u, 0x33u, 0x44u};
    pl::FillNops(patch_buf.data(), patch_buf.size());
    const std::array<std::uint8_t, 4> expected_nops = {0x90u, 0x90u, 0x90u, 0x90u};
    EXPECT_EQ(patch_buf, expected_nops);
    EXPECT_TRUE(pl::BytesMatch(patch_buf.data(), expected_nops.data(), patch_buf.size()));
    const std::uint8_t two_bytes[2] = {0xABu, 0xCDu};
    EXPECT_EQ(pl::EmitBytes(patch_buf.data(), two_bytes, 2), patch_buf.data() + 2);
    EXPECT_FALSE(pl::BytesMatch(patch_buf.data(), expected_nops.data(), patch_buf.size()));
    EXPECT_FALSE(pl::BytesMatch(nullptr, expected_nops.data(), 1));

    // MatchesTextRenderCorePrologue accept/reject boundary matrix.
    const std::uint8_t valid_prologue[pl::kTextRenderCoreHookLen] = {
        0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xDC, 0x01, 0x00, 0x00};
    const std::uint8_t bad_prefix[pl::kTextRenderCoreHookLen] = {
        0x55, 0x8B, 0xEC, 0x83, 0xEC, 0xDC, 0x01, 0x00, 0x00};
    const std::uint8_t bad_sub_hi[pl::kTextRenderCoreHookLen] = {
        0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xDC, 0x02, 0x00, 0x00};
    const std::uint8_t bad_sub_b2[pl::kTextRenderCoreHookLen] = {
        0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xDC, 0x01, 0x01, 0x00};
    const std::uint8_t bad_sub_b3[pl::kTextRenderCoreHookLen] = {
        0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xDC, 0x01, 0x00, 0x80};
    EXPECT_TRUE(pl::MatchesTextRenderCorePrologue(valid_prologue));
    EXPECT_FALSE(pl::MatchesTextRenderCorePrologue(bad_prefix));
    EXPECT_FALSE(pl::MatchesTextRenderCorePrologue(bad_sub_hi));
    EXPECT_FALSE(pl::MatchesTextRenderCorePrologue(bad_sub_b2));
    EXPECT_FALSE(pl::MatchesTextRenderCorePrologue(bad_sub_b3));
    EXPECT_FALSE(pl::MatchesTextRenderCorePrologue(nullptr));

    // LooksLikeAscii: 4-byte DWORD containing >= 3 printable ASCII (0x20..0x7E) + NUL only.
    EXPECT_FALSE(pl::LooksLikeAscii(0x00000000u));  // 0 printable bytes
    EXPECT_FALSE(pl::LooksLikeAscii(0x00004241u));  // 2 printable + 2 NUL (below min 3)
    EXPECT_TRUE(pl::LooksLikeAscii(0x007E4120u));   // 3 printable (0x20, 'A', 0x7E) + 1 NUL
    EXPECT_TRUE(pl::LooksLikeAscii(0x44434241u));   // 4 printable ("ABCD")
    EXPECT_FALSE(pl::LooksLikeAscii(0x1F434241u));  // control byte 0x1F rejected
    EXPECT_FALSE(pl::LooksLikeAscii(0x7F434241u));  // DEL byte 0x7F rejected
    EXPECT_FALSE(pl::LooksLikeAscii(0x80434241u));  // high-bit byte 0x80 rejected
    EXPECT_FALSE(pl::LooksLikeAscii(0xFF434241u));  // 0xFF rejected
}
