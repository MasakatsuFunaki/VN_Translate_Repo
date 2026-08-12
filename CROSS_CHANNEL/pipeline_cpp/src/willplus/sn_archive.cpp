// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "sn_archive.h"

#include <array>
#include <stdexcept>

namespace crc::willplus {

std::uint32_t sn_expected_size(const Bytes& data) {
    if (data.size() < 4) throw std::runtime_error("sn.bin too small for the LE32 size header");
    return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8) |
           (static_cast<std::uint32_t>(data[2]) << 16) | (static_cast<std::uint32_t>(data[3]) << 24);
}

Bytes lzss_decompress(const Bytes& data) {
    const std::uint32_t expected = sn_expected_size(data);
    const std::uint8_t* src = data.data() + 4;
    const std::size_t src_len = data.size() - 4;

    std::array<std::uint8_t, 4096> ring{};
    ring.fill(0x20);
    std::size_t ring_pos = 0xFEE;

    Bytes out;
    out.reserve(expected);  // hint only -- the output is never clamped to it

    std::size_t si = 0;
    while (si < src_len) {
        const std::uint8_t flags = src[si++];
        for (int bit = 0; bit < 8; ++bit) {
            // Checked at the TOP of every bit, not once per flag byte.
            if (si >= src_len) return out;
            if (flags & (1u << bit)) {
                const std::uint8_t b = src[si++];
                out.push_back(b);
                ring[ring_pos] = b;
                ring_pos = (ring_pos + 1) & 0xFFF;
            } else {
                if (si + 1 >= src_len) return out;
                const std::uint8_t b1 = src[si];
                const std::uint8_t b2 = src[si + 1];
                si += 2;
                const std::size_t ref_off =
                    static_cast<std::size_t>(b1) | (static_cast<std::size_t>(b2 & 0xF0) << 4);
                const int ref_len = (b2 & 0x0F) + 3;
                for (int j = 0; j < ref_len; ++j) {
                    const std::uint8_t b = ring[(ref_off + static_cast<std::size_t>(j)) & 0xFFF];
                    out.push_back(b);
                    ring[ring_pos] = b;
                    ring_pos = (ring_pos + 1) & 0xFFF;
                }
            }
        }
    }
    return out;
}

}  // namespace crc::willplus
