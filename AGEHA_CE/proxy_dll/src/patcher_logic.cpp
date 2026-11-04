// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

#include "patcher_logic.h"

#include <cstring>

namespace patcher_logic {

namespace {

constexpr std::size_t kDwordByteCount = 4;
constexpr std::uint8_t kAsciiPrintableMin = 0x20;
constexpr std::uint8_t kAsciiPrintableMax = 0x7E;
constexpr int kMinPrintableBytes = 3;

}  // namespace

bool LooksLikeAscii(std::uint32_t val) {
    const auto* b = reinterpret_cast<const std::uint8_t*>(&val);
    int printable = 0;
    for (std::size_t i = 0; i < kDwordByteCount; ++i) {
        if (b[i] >= kAsciiPrintableMin && b[i] <= kAsciiPrintableMax) {
            ++printable;
            continue;
        }
        if (b[i] != 0) {
            return false;
        }
    }
    return printable >= kMinPrintableBytes;
}

bool BytesMatch(const std::uint8_t* addr,
                const std::uint8_t* expected,
                std::size_t len) {
    if (len == 0) return true;
    if (!addr || !expected) return false;
    return std::memcmp(addr, expected, len) == 0;
}

void FillNops(std::uint8_t* dst, std::size_t len) {
    if (!dst || len == 0) return;
    std::memset(dst, kNopOpcode, len);
}

std::uint8_t* EmitBytes(std::uint8_t* cursor,
                        const std::uint8_t* bytes,
                        std::size_t len) {
    if (!cursor) return nullptr;
    if (bytes && len > 0) {
        std::memcpy(cursor, bytes, len);
    }
    return cursor + len;
}

std::int32_t Rel32Offset(std::uintptr_t nextIp, std::uintptr_t target) {
    return static_cast<std::int32_t>(
        static_cast<std::uint32_t>(target) -
        static_cast<std::uint32_t>(nextIp));
}

std::uint8_t* EmitRelCall32(std::uint8_t* cursor, std::uintptr_t target) {
    if (!cursor) return nullptr;
    *cursor++ = kRelCallOpcode;
    const std::int32_t rel =
        Rel32Offset(reinterpret_cast<std::uintptr_t>(cursor + kDwordByteCount), target);
    std::memcpy(cursor, &rel, sizeof(rel));
    return cursor + kDwordByteCount;
}

std::uint8_t* EmitRelJmp32(std::uint8_t* cursor, std::uintptr_t target) {
    if (!cursor) return nullptr;
    *cursor++ = kRelJmpOpcode;
    const std::int32_t rel =
        Rel32Offset(reinterpret_cast<std::uintptr_t>(cursor + kDwordByteCount), target);
    std::memcpy(cursor, &rel, sizeof(rel));
    return cursor + kDwordByteCount;
}

}  // namespace patcher_logic
