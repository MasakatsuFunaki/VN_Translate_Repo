// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Pinned XTX substitution tables for WelcomeToAnzU_CE.
// The EN side is full-width romaji on purpose: the BLACKCyc engine crashes
// on single-byte ASCII in XTX display fields.
#include "xtx.h"

namespace exm::xtx {

namespace {

constexpr std::uint8_t kCipherXorStep = 0x5Au;
constexpr std::uint8_t kCipherRotateBits = 3u;
constexpr std::uint8_t kBitsInByte = 8u;

std::uint8_t encode_byte(std::size_t plain) noexcept {
    const auto b = static_cast<std::uint8_t>(plain);
    const auto rotated = static_cast<std::uint8_t>(
        (b << kCipherRotateBits) | (b >> (kBitsInByte - kCipherRotateBits)));
    return static_cast<std::uint8_t>(rotated ^ kCipherXorStep);
}

}  // namespace

const std::array<std::uint8_t, kXtxByteTableSize>& xtx_encode_table() {
    static const std::array<std::uint8_t, kXtxByteTableSize> table = [] {
        std::array<std::uint8_t, kXtxByteTableSize> t{};
        for (std::size_t i = 0; i < kXtxByteTableSize; ++i) {
            t[i] = encode_byte(i);
        }
        return t;
    }();
    return table;
}

const std::array<std::uint8_t, kXtxByteTableSize>& xtx_decode_table() {
    static const std::array<std::uint8_t, kXtxByteTableSize> table = [] {
        std::array<std::uint8_t, kXtxByteTableSize> t{};
        const auto& enc = xtx_encode_table();
        for (std::size_t i = 0; i < kXtxByteTableSize; ++i) {
            t[enc[i]] = static_cast<std::uint8_t>(i);
        }
        return t;
    }();
    return table;
}

}  // namespace exm::xtx
