// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// BGI CompressedBG v1 image decoder + BMP encoder (GARbro / asmodean).
// v2 (DCT) not implemented; callers fall back to TOOLS/garbro.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/util.h"

namespace snu::bgi {

struct CbgImage {
    Bytes pixels;  // row-major BGR(A)
    int width = 0;
    int height = 0;
    int bpp = 0;
};

inline bool is_cbg(const Bytes& raw) {
    static const char magic[] = "CompressedBG___";
    return raw.size() >= 16 && std::memcmp(raw.data(), magic, 16) == 0;
}

// Decode a CompressedBG blob.  Throws std::runtime_error on bad magic or
// leaf-table checksum mismatch.
CbgImage decode_cbg(const Bytes& data);

// Wrap raw row-major BGR(A) bytes into a bottom-up Windows BMP.
Bytes to_bmp(const Bytes& pixels_bgr, int width, int height, int bpp);

}  // namespace snu::bgi
