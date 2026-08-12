// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// sn.bin container reader.
//
// WillPlus/AdvHD stores the whole script archive as
//     [0..4)  uint32 LE  decompressed size (informational only)
//     [4..)   LZSS stream, cc-fkb-tools-rs flavour
// There is no encryption anywhere in this title.
#pragma once

#include <cstdint>

#include "common/util.h"

namespace crc::willplus {

// LE32 at offset 0.  Advisory only -- it is logged and sanity-checked, but the
// decompressor never truncates or pads its output to match it.
std::uint32_t sn_expected_size(const Bytes& data);

// 4096-byte ring pre-filled with 0x20, ring_pos 0xFEE, LSB-first flag bits
// (1 = literal, 0 = 12-bit offset / 4-bit length back-reference, min match 3).
// A truncated stream returns the partial output rather than throwing: running
// out of bits mid-group is a normal end-of-stream here, not corruption.
Bytes lzss_decompress(const Bytes& data);

}  // namespace crc::willplus
