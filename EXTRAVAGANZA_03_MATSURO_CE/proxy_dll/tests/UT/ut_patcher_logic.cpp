// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Unit tests for proxy_dll/src/patcher_logic.{h,cpp}: byte matching, NOP
// filling, relative branch encoding, and EXTRAVAGANZA_03_MATSURO_CE hook/patch
// constants.
#include "patcher_logic.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace pl = patcher_logic;

namespace {

constexpr std::uint32_t kAsciiWordSpt = 0x00747073;
constexpr std::uint32_t kNonAsciiWord = 0x82A082A2;
constexpr std::size_t kTestNopBufferSize = 32;
constexpr std::uintptr_t kSampleBranchOrigin = 0x004FF140;
constexpr std::uintptr_t kSampleBranchTarget = 0x00500000;
constexpr std::size_t kBranchInstrBytes = 5;
constexpr std::int32_t kExpectedRel32 =
    static_cast<std::int32_t>(kSampleBranchTarget - (kSampleBranchOrigin + kBranchInstrBytes));

}  // namespace

TEST(PatcherLogic, looks_like_ascii_identifies_printable_dwords) {
    EXPECT_TRUE(pl::LooksLikeAscii(kAsciiWordSpt));
    EXPECT_FALSE(pl::LooksLikeAscii(kNonAsciiWord));
    EXPECT_FALSE(pl::LooksLikeAscii(0));
}

TEST(PatcherLogic, bytes_match_and_fill_nops) {
    std::vector<std::uint8_t> buf(kTestNopBufferSize, 0x00);
    pl::FillNops(buf.data(), kTestNopBufferSize);
    for (std::uint8_t b : buf) {
        EXPECT_EQ(b, pl::kNopOpcode);
    }
    EXPECT_TRUE(pl::BytesMatch(buf.data(), buf.data(), kTestNopBufferSize));
    EXPECT_FALSE(pl::BytesMatch(nullptr, buf.data(), kTestNopBufferSize));
}

TEST(PatcherLogic, emit_rel_call_and_jmp_encode_exact_5_byte_branches) {
    std::uint8_t callBuf[pl::kRelBranchInstructionSize] = {};
    std::uint8_t* callEnd = pl::EmitRelCall32(callBuf, kSampleBranchTarget);
    EXPECT_EQ(callEnd, callBuf + pl::kRelBranchInstructionSize);
    EXPECT_EQ(callBuf[0], pl::kRelCallOpcode);

    std::uint8_t jmpBuf[pl::kRelBranchInstructionSize] = {};
    std::uint8_t* jmpEnd = pl::EmitRelJmp32(jmpBuf, kSampleBranchTarget);
    EXPECT_EQ(jmpEnd, jmpBuf + pl::kRelBranchInstructionSize);
    EXPECT_EQ(jmpBuf[0], pl::kRelJmpOpcode);

    EXPECT_EQ(pl::Rel32Offset(kSampleBranchOrigin + pl::kRelBranchInstructionSize,
                              kSampleBranchTarget),
              kExpectedRel32);
}

TEST(PatcherLogic, matsuro_ce_rva_and_sentinel_constants_match_contract) {
    EXPECT_EQ(pl::kTextSplitterRva, 0x0FF140u);
    EXPECT_EQ(pl::kTextSplitterSentinelLen, 6u);
    EXPECT_EQ(pl::kTextSplitterHookLen, 9u);
    EXPECT_TRUE(pl::BytesMatch(pl::kTextSplitterPrologue,
                               pl::kTextSplitterSentinel,
                               pl::kTextSplitterSentinelLen));

    EXPECT_EQ(pl::kPrintMessageSentinelRva, 0x100D11u);
    EXPECT_EQ(pl::kPrintMessageSentinelLen, 10u);
    EXPECT_EQ(pl::kPatch1FontSizeRva, 0x100D17u);
    EXPECT_EQ(pl::kOrigFontSize, 0x18u);
    EXPECT_EQ(pl::kPatchedFontSize, 0x0Eu);
    EXPECT_EQ(pl::kPatch2NextYAddendRva, 0x100D45u);
    EXPECT_EQ(pl::kPatch3FxfFontSizeRva, 0x100D1Bu);
    EXPECT_EQ(pl::kPatch3FxfFontSizeNopLen, 28u);
    EXPECT_EQ(pl::kPatch4FxfNextYRva, 0x100DA8u);
    EXPECT_EQ(pl::kPatch4FxfNextYNopLen, 29u);
    EXPECT_EQ(pl::kPatch5FxfLNextYRva, 0x100E97u);
    EXPECT_EQ(pl::kPatch5FxfLNextYNopLen, 28u);
    EXPECT_EQ(pl::kPatch6SpaceDefaultRva, 0x100DD1u);
    EXPECT_EQ(pl::kPatch7FxfSpaceRva, 0x100DD5u);
    EXPECT_EQ(pl::kPatch7FxfSpaceNopLen, 28u);
    EXPECT_EQ(pl::kPatch8FxfNextXRva, 0x100D8Cu);
    EXPECT_EQ(pl::kPatch8FxfNextXNopLen, 28u);
    EXPECT_EQ(pl::kPatch9WindowMsgNumRva, 0x1010D5u);
    EXPECT_EQ(pl::kOrigWindowMsgNum, 0x04u);
    EXPECT_EQ(pl::kPatchedWindowMsgNum, 0x07u);
    EXPECT_EQ(pl::kPatch10FxfWindowMsgNumRva, 0x101104u);
    EXPECT_EQ(pl::kPatch10FxfWindowMsgNumNopLen, 28u);

    EXPECT_EQ(pl::kTextRenderCoreRva, 0x03B870u);
    EXPECT_EQ(pl::kAltTextRenderCoreRva, 0x02D480u);
    EXPECT_EQ(pl::kFontSetupRva, 0x03E890u);
    EXPECT_EQ(pl::kAltFontSetupRva, 0x031790u);
    EXPECT_EQ(pl::kFontMgrActiveFlagOffset, 0x130u);

    const std::uint8_t familyAPrologue[pl::kTextRenderCoreHookLen] = {
        0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xDC, 0x01, 0x00, 0x00};
    const std::uint8_t familyBPrologue[pl::kTextRenderCoreHookLen] = {
        0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xFC, 0x01, 0x00, 0x00};
    const std::uint8_t badPrologue[pl::kTextRenderCoreHookLen] = {
        0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08, 0x00, 0x00, 0x00};
    EXPECT_TRUE(pl::MatchesTextRenderCorePrologue(familyAPrologue));
    EXPECT_TRUE(pl::MatchesTextRenderCorePrologue(familyBPrologue));
    EXPECT_FALSE(pl::MatchesTextRenderCorePrologue(badPrologue));
    EXPECT_FALSE(pl::MatchesTextRenderCorePrologue(nullptr));
}
