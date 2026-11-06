// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Pure, Win32-free byte/instruction helpers and YUMEMIRYOJOKU_CE RVA constants for
// the runtime patcher and TextSplitter hook.
#pragma once

#include <cstddef>
#include <cstdint>

namespace patcher_logic {

constexpr std::uint32_t kTextSplitterRva = 0x0F7650;
constexpr std::uint32_t kFallbackTextSplitterRva = 0x0F6DE0;
constexpr std::uint32_t kAltTextSplitterRva = 0x0FF140;
constexpr std::uint32_t kRelTextSplitterFromSentinel = 0x1361;
constexpr std::uint32_t kAltRelTextSplitterFromSentinel = 0x1BD1;
constexpr std::size_t kTextSplitterSentinelLen = 6;
constexpr std::size_t kTextSplitterHookLen = 9;
inline constexpr std::uint8_t kTextSplitterSentinel[kTextSplitterSentinelLen] = {
    0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x5C,
};
inline constexpr std::uint8_t kTextSplitterPrologue[kTextSplitterHookLen] = {
    0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x5C, 0x01, 0x00, 0x00,
};

constexpr std::uint32_t kTextRenderCoreRva = 0x02D480;
constexpr std::uint32_t kAltTextRenderCoreRva = 0x03B870;
constexpr std::size_t kTextRenderCorePrefixLen = 5;
constexpr std::size_t kTextRenderCoreHookLen = 9;
inline constexpr std::uint8_t kTextRenderCorePrefix[kTextRenderCorePrefixLen] = {
    0x55, 0x8B, 0xEC, 0x81, 0xEC,
};
constexpr std::size_t kSubEspImm32Byte1Index = 6;
constexpr std::size_t kSubEspImm32Byte2Index = 7;
constexpr std::size_t kSubEspImm32Byte3Index = 8;
constexpr std::uint8_t kTextRenderCoreSubEspHighByte = 0x01;

constexpr std::uint32_t kFontSetupRva = 0x031790;
constexpr std::uint32_t kAltFontSetupRva = 0x03E890;
constexpr std::size_t kFontSetupPrologueLen = 9;
inline constexpr std::uint8_t kFontSetupPrologue[kFontSetupPrologueLen] = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C, 0x89, 0x4D, 0xF4,
};
constexpr std::size_t kFontMgrActiveFlagOffset = 0x130;

constexpr std::uint32_t kPrintMessageSentinelRva = 0x0F89B1;
constexpr std::uint32_t kAltPrintMessageSentinelRva = 0x100D11;
constexpr std::uint32_t kTextSectionScanStartRva = 0x001000;
constexpr std::uint32_t kTextSectionScanEndRva = 0x15B000;
constexpr std::size_t kPrintMessageSentinelLen = 10;
inline constexpr std::uint8_t kPrintMessageSentinel[kPrintMessageSentinelLen] = {
    0xC7, 0x82, 0x48, 0x46, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00,
};

constexpr std::uint32_t kRelPatch1FontSize = 0x006;
constexpr std::uint32_t kRelPatch2NextYAddend = 0x034;
constexpr std::uint32_t kRelPatch3FxfFontSize = 0x00A;
constexpr std::uint32_t kRelPatch4FxfNextY = 0x097;
constexpr std::uint32_t kRelPatch5FxfLNextY = 0x186;
constexpr std::uint32_t kRelPatch6SpaceDefault = 0x0C0;
constexpr std::uint32_t kRelPatch7FxfSpace = 0x0C4;
constexpr std::uint32_t kRelPatch8FxfNextX = 0x07B;
constexpr std::uint32_t kRelPatch9WindowMsgNum = 0x3C4;
constexpr std::uint32_t kRelPatch10FxfWindowMsgNum = 0x3F3;

constexpr std::uint32_t kPatch1FontSizeRva = kPrintMessageSentinelRva + kRelPatch1FontSize;
constexpr std::uint8_t kOrigFontSize = 0x18;
constexpr std::uint8_t kPatchedFontSize = 0x0E;

constexpr std::uint32_t kPatch2NextYAddendRva = kPrintMessageSentinelRva + kRelPatch2NextYAddend;
constexpr std::uint8_t kOrigNextYAddend = 0x06;
constexpr std::uint8_t kPatchedNextYAddend = 0x04;

constexpr std::uint32_t kPatch3FxfFontSizeRva = kPrintMessageSentinelRva + kRelPatch3FxfFontSize;
constexpr std::size_t kPatch3FxfFontSizeNopLen = 28;

constexpr std::uint32_t kPatch4FxfNextYRva = kPrintMessageSentinelRva + kRelPatch4FxfNextY;
constexpr std::size_t kPatch4FxfNextYNopLen = 29;

constexpr std::uint32_t kPatch5FxfLNextYRva = kPrintMessageSentinelRva + kRelPatch5FxfLNextY;
constexpr std::size_t kPatch5FxfLNextYNopLen = 28;

constexpr std::uint32_t kPatch6SpaceDefaultRva = kPrintMessageSentinelRva + kRelPatch6SpaceDefault;
constexpr std::size_t kSpaceImmediateLen = 4;
inline constexpr std::uint8_t kOrigSpaceImmediate[kSpaceImmediateLen] = {
    0x01, 0x00, 0x00, 0x00,
};
inline constexpr std::uint8_t kPatchedSpaceImmediate[kSpaceImmediateLen] = {
    0xFC, 0xFF, 0xFF, 0xFF,
};

constexpr std::uint32_t kPatch7FxfSpaceRva = kPrintMessageSentinelRva + kRelPatch7FxfSpace;
constexpr std::size_t kPatch7FxfSpaceNopLen = 28;

constexpr std::uint32_t kPatch8FxfNextXRva = kPrintMessageSentinelRva + kRelPatch8FxfNextX;
constexpr std::size_t kPatch8FxfNextXNopLen = 28;

constexpr std::uint32_t kPatch9WindowMsgNumRva = kPrintMessageSentinelRva + kRelPatch9WindowMsgNum;
constexpr std::uint8_t kOrigWindowMsgNum = 0x04;
constexpr std::uint8_t kPatchedWindowMsgNum = 0x07;

constexpr std::uint32_t kPatch10FxfWindowMsgNumRva =
    kPrintMessageSentinelRva + kRelPatch10FxfWindowMsgNum;
constexpr std::size_t kPatch10FxfWindowMsgNumNopLen = 28;

constexpr std::uint8_t kPushImm32Opcode = 0x68;
constexpr std::uint8_t kNopOpcode = 0x90;
constexpr std::uint8_t kRelCallOpcode = 0xE8;
constexpr std::uint8_t kRelJmpOpcode = 0xE9;
constexpr std::size_t kRelBranchInstructionSize = 5;

bool LooksLikeAscii(std::uint32_t val);

bool BytesMatch(const std::uint8_t* addr,
                const std::uint8_t* expected,
                std::size_t len);

bool MatchesTextRenderCorePrologue(const std::uint8_t* addr);

void FillNops(std::uint8_t* dst, std::size_t len);

std::uint8_t* EmitBytes(std::uint8_t* cursor,
                        const std::uint8_t* bytes,
                        std::size_t len);

std::int32_t Rel32Offset(std::uintptr_t nextIp, std::uintptr_t target);

std::uint8_t* EmitRelCall32(std::uint8_t* cursor, std::uintptr_t target);

std::uint8_t* EmitRelJmp32(std::uint8_t* cursor, std::uintptr_t target);

}  // namespace patcher_logic
