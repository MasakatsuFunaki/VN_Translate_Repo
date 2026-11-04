// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Pure, Win32-free byte/instruction helpers and AGEHA_CE RVA constants for
// the runtime patcher and TextSplitter hook.
#pragma once

#include <cstddef>
#include <cstdint>

namespace patcher_logic {

constexpr std::uint32_t kTextSplitterRva = 0x0FF140;
constexpr std::size_t kTextSplitterSentinelLen = 6;
constexpr std::size_t kTextSplitterHookLen = 9;
inline constexpr std::uint8_t kTextSplitterSentinel[kTextSplitterSentinelLen] = {
    0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x5C,
};
inline constexpr std::uint8_t kTextSplitterPrologue[kTextSplitterHookLen] = {
    0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x5C, 0x01, 0x00, 0x00,
};

constexpr std::uint32_t kPrintMessageSentinelRva = 0x100D11;
constexpr std::size_t kPrintMessageSentinelLen = 10;
inline constexpr std::uint8_t kPrintMessageSentinel[kPrintMessageSentinelLen] = {
    0xC7, 0x82, 0x48, 0x46, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00,
};

constexpr std::uint32_t kPatch1FontSizeRva = 0x100D17;
constexpr std::uint8_t kOrigFontSize = 0x18;
constexpr std::uint8_t kPatchedFontSize = 0x0E;

constexpr std::uint32_t kPatch2NextYAddendRva = 0x100D45;
constexpr std::uint8_t kOrigNextYAddend = 0x06;
constexpr std::uint8_t kPatchedNextYAddend = 0x04;

constexpr std::uint32_t kPatch3FxfFontSizeRva = 0x100D1B;
constexpr std::size_t kPatch3FxfFontSizeNopLen = 28;

constexpr std::uint32_t kPatch4FxfNextYRva = 0x100DA8;
constexpr std::size_t kPatch4FxfNextYNopLen = 29;

constexpr std::uint32_t kPatch5FxfLNextYRva = 0x100E97;
constexpr std::size_t kPatch5FxfLNextYNopLen = 28;

constexpr std::uint32_t kPatch6SpaceDefaultRva = 0x100DD1;
constexpr std::size_t kSpaceImmediateLen = 4;
inline constexpr std::uint8_t kOrigSpaceImmediate[kSpaceImmediateLen] = {
    0x01, 0x00, 0x00, 0x00,
};
inline constexpr std::uint8_t kPatchedSpaceImmediate[kSpaceImmediateLen] = {
    0xFC, 0xFF, 0xFF, 0xFF,
};

constexpr std::uint32_t kPatch7FxfSpaceRva = 0x100DD5;
constexpr std::size_t kPatch7FxfSpaceNopLen = 28;

constexpr std::uint32_t kPatch8FxfNextXRva = 0x100D8C;
constexpr std::size_t kPatch8FxfNextXNopLen = 28;

constexpr std::uint32_t kPatch9WindowMsgNumRva = 0x1010D5;
constexpr std::uint8_t kOrigWindowMsgNum = 0x04;
constexpr std::uint8_t kPatchedWindowMsgNum = 0x07;

constexpr std::uint32_t kPatch10FxfWindowMsgNumRva = 0x101104;
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

void FillNops(std::uint8_t* dst, std::size_t len);

std::uint8_t* EmitBytes(std::uint8_t* cursor,
                        const std::uint8_t* bytes,
                        std::size_t len);

std::int32_t Rel32Offset(std::uintptr_t nextIp, std::uintptr_t target);

std::uint8_t* EmitRelCall32(std::uint8_t* cursor, std::uintptr_t target);

std::uint8_t* EmitRelJmp32(std::uint8_t* cursor, std::uintptr_t target);

}  // namespace patcher_logic
