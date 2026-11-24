// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// BGI / Ethornell uncompressed system-graphic (`sysgrp.arc` inner payload)
// codec: 16-byte header (`[u16 width, u16 height, u16 bpp, 10 reserved]`)
// followed by top-down 24-bpp BGR or 32-bpp straight-BGRA scanlines without
// row padding.
#pragma once

#include "common/util.h"
#include "menu/bgra_image.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace snu::bgi {

inline constexpr std::size_t kSysGrpHeaderBytes = 16;
inline constexpr std::size_t kSysGrpReservedBytes = 10;

struct SysGrpImage {
    snu::menu::BgraImage image;
    int orig_bpp = snu::menu::kBmpBpp32;
    std::array<std::uint8_t, kSysGrpReservedBytes> reserved{};
};

// Parses a decompressed `sysgrp.arc` graphic payload into a top-down 32-bit
// straight-BGRA image (`a = 255` when `bpp == 24`).
// Throws std::runtime_error on invalid dimensions, unsupported bpp, or size
// mismatch.
SysGrpImage decode_sysgrp(const Bytes& decompressed_raw);

// Serializes `image` into a 16-byte sysgrp header + top-down 24-bpp BGR or
// 32-bpp BGRA pixel payload.
// Throws std::runtime_error on empty/oversized dimensions or unsupported bpp.
Bytes encode_sysgrp(const snu::menu::BgraImage& image, int bpp,
                    const std::array<std::uint8_t, kSysGrpReservedBytes>& reserved = {});

}  // namespace snu::bgi
