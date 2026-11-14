// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "siglus/siglus_decoder.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace adi::siglus {

namespace {

constexpr std::size_t kUniversalKeyMask = 0xFFu;
constexpr std::size_t kPrivateKeyMask = 0x0Fu;
constexpr int kBitsPerFlagByte = 8;
constexpr std::uint8_t kLiteralBitMask = 0x01u;
constexpr unsigned kFlagBitShift = 1u;
constexpr std::int64_t kBackrefPairBytes = 2;
constexpr unsigned kByteShift8 = 8u;
constexpr unsigned kByteShift16 = 16u;
constexpr unsigned kByteShift24 = 24u;
constexpr std::uint32_t kBackrefLenMask = 0x0Fu;
constexpr std::int64_t kBackrefMinLength = 2;
constexpr unsigned kBackrefOffsetShift = 4u;
constexpr std::size_t kChunkHeaderBytes = 8u;
constexpr std::size_t kChunkDecompOffset = 4u;
constexpr std::uint32_t kMaxPlausibleDecompBytes = 0x10000000u;
constexpr std::size_t kErrorBufSize = 96u;

constexpr std::size_t kMinSceneHeaderV2Bytes = 0x1Cu;
constexpr std::size_t kStrIdxOffsetField = 0x0Cu;
constexpr std::size_t kStrIdxCountField = 0x10u;
constexpr std::size_t kStrBlobOffsetField = 0x14u;
constexpr std::uint32_t kMaxSceneStringCount = 100000u;
constexpr std::uint64_t kIndexEntryStrideBytes = 8u;
constexpr std::size_t kIndexLengthFieldOffset = 4u;
constexpr std::uint32_t kMaxStringUnits = 4096u;
constexpr std::size_t kBytesPerUtf16Unit = 2u;
constexpr std::uint16_t kSsV2KeyMask = 0xFFFFu;

struct RawEntry {
    bool present = false;
    std::size_t off = 0;
    std::size_t len = 0;
};

std::uint32_t read_u32_le(const Bytes& b, std::size_t off) {
    return static_cast<std::uint32_t>(b[off]) |
           (static_cast<std::uint32_t>(b[off + 1]) << kByteShift8) |
           (static_cast<std::uint32_t>(b[off + 2]) << kByteShift16) |
           (static_cast<std::uint32_t>(b[off + 3]) << kByteShift24);
}

std::uint16_t read_unit_le(const Bytes& b, std::size_t off) {
    return static_cast<std::uint16_t>(
        b[off] | (static_cast<std::uint16_t>(b[off + 1]) << kByteShift8));
}

bool read_raw_entries(const Bytes& d, std::vector<RawEntry>& out) {
    if (d.size() < kMinSceneHeaderV2Bytes) {
        return false;
    }
    const std::uint32_t idx_off = read_u32_le(d, kStrIdxOffsetField);
    const std::uint32_t idx_cnt = read_u32_le(d, kStrIdxCountField);
    const std::uint32_t data_off = read_u32_le(d, kStrBlobOffsetField);

    if (idx_cnt == 0 || idx_cnt > kMaxSceneStringCount) {
        return false;
    }
    if (static_cast<std::uint64_t>(idx_off) +
            static_cast<std::uint64_t>(idx_cnt) * kIndexEntryStrideBytes >
        d.size()) {
        return false;
    }
    if (data_off >= d.size()) {
        return false;
    }

    out.reserve(idx_cnt);
    for (std::uint32_t j = 0; j < idx_cnt; ++j) {
        const std::size_t e =
            static_cast<std::size_t>(idx_off) +
            static_cast<std::size_t>(j) * static_cast<std::size_t>(kIndexEntryStrideBytes);
        const std::uint32_t woff = read_u32_le(d, e);
        const std::uint32_t wlen = read_u32_le(d, e + kIndexLengthFieldOffset);
        RawEntry r;
        if (wlen == 0 || wlen > kMaxStringUnits) {
            out.push_back(r);
            continue;
        }
        const std::uint64_t s =
            static_cast<std::uint64_t>(data_off) +
            static_cast<std::uint64_t>(woff) * kBytesPerUtf16Unit;
        const std::uint64_t end =
            s + static_cast<std::uint64_t>(wlen) * kBytesPerUtf16Unit;
        if (end > d.size()) {
            out.push_back(r);
            continue;
        }
        r.present = true;
        r.off = static_cast<std::size_t>(s);
        r.len = static_cast<std::size_t>(wlen) * kBytesPerUtf16Unit;
        out.push_back(r);
    }
    return true;
}

}  // namespace

// Both key tables below are machine-emitted, never retyped -- a single wrong
// nibble decodes every scene to noise.  ut_siglus_decoder pins them
// (first/last byte + checksum), and the RealScenePck* cases fail loudly if
// either table drifts.
const std::array<std::uint8_t, kSiglusKey256Size> SIGLUS_KEY_256 = {{
    0x70, 0xF8, 0xA6, 0xB0, 0xA1, 0xA5, 0x28, 0x4F, 0xB5, 0x2F, 0x48, 0xFA, 0xE1, 0xE9, 0x4B, 0xDE,
    0xB7, 0x4F, 0x62, 0x95, 0x8B, 0xE0, 0x03, 0x80, 0xE7, 0xCF, 0x0F, 0x6B, 0x92, 0x01, 0xEB, 0xF8,
    0xA2, 0x88, 0xCE, 0x63, 0x04, 0x38, 0xD2, 0x6D, 0x8C, 0xD2, 0x88, 0x76, 0xA7, 0x92, 0x71, 0x8F,
    0x4E, 0xB6, 0x8D, 0x01, 0x79, 0x88, 0x83, 0x0A, 0xF9, 0xE9, 0x2C, 0xDB, 0x67, 0xDB, 0x91, 0x14,
    0xD5, 0x9A, 0x4E, 0x79, 0x17, 0x23, 0x08, 0x96, 0x0E, 0x1D, 0x15, 0xF9, 0xA5, 0xA0, 0x6F, 0x58,
    0x17, 0xC8, 0xA9, 0x46, 0xDA, 0x22, 0xFF, 0xFD, 0x87, 0x12, 0x42, 0xFB, 0xA9, 0xB8, 0x67, 0x6C,
    0x91, 0x67, 0x64, 0xF9, 0xD1, 0x1E, 0xE4, 0x50, 0x64, 0x6F, 0xF2, 0x0B, 0xDE, 0x40, 0xE7, 0x47,
    0xF1, 0x03, 0xCC, 0x2A, 0xAD, 0x7F, 0x34, 0x21, 0xA0, 0x64, 0x26, 0x98, 0x6C, 0xED, 0x69, 0xF4,
    0xB5, 0x23, 0x08, 0x6E, 0x7D, 0x92, 0xF6, 0xEB, 0x93, 0xF0, 0x7A, 0x89, 0x5E, 0xF9, 0xF8, 0x7A,
    0xAF, 0xE8, 0xA9, 0x48, 0xC2, 0xAC, 0x11, 0x6B, 0x2B, 0x33, 0xA7, 0x40, 0x0D, 0xDC, 0x7D, 0xA7,
    0x5B, 0xCF, 0xC8, 0x31, 0xD1, 0x77, 0x52, 0x8D, 0x82, 0xAC, 0x41, 0xB8, 0x73, 0xA5, 0x4F, 0x26,
    0x7C, 0x0F, 0x39, 0xDA, 0x5B, 0x37, 0x4A, 0xDE, 0xA4, 0x49, 0x0B, 0x7C, 0x17, 0xA3, 0x43, 0xAE,
    0x77, 0x06, 0x64, 0x73, 0xC0, 0x43, 0xA3, 0x18, 0x5A, 0x0F, 0x9F, 0x02, 0x4C, 0x7E, 0x8B, 0x01,
    0x9F, 0x2D, 0xAE, 0x72, 0x54, 0x13, 0xFF, 0x96, 0xAE, 0x0B, 0x34, 0x58, 0xCF, 0xE3, 0x00, 0x78,
    0xBE, 0xE3, 0xF5, 0x61, 0xE4, 0x87, 0x7C, 0xFC, 0x80, 0xAF, 0xC4, 0x8D, 0x46, 0x3A, 0x5D, 0xD0,
    0x36, 0xBC, 0xE5, 0x60, 0x77, 0x68, 0x08, 0x4F, 0xBB, 0xAB, 0xE2, 0x78, 0x07, 0xE8, 0x73, 0xBF,
}};

const std::array<std::uint8_t, kPrivateKeySize> ADIEU_PRIVATE_KEY = {{
    0xA2, 0x7E, 0xC9, 0xDA, 0x2E, 0x2D, 0x09, 0x08, 0xE5, 0xEA, 0xFA, 0x96, 0xA3, 0x1B, 0xED, 0xE0,
}};

const std::array<std::uint8_t, kPartialKeySize> ADIEU_PRIVATE_KEY_PARTIAL = {{
    0xA2, 0x7E, 0xC9, 0xDA, 0x7D, 0x1E, 0x09, 0x08,
}};

Bytes decrypt_universal(const Bytes& buf) {
    Bytes out(buf.size());
    for (std::size_t i = 0; i < buf.size(); ++i) {
        out[i] = static_cast<std::uint8_t>(buf[i] ^ SIGLUS_KEY_256[i & kUniversalKeyMask]);
    }
    return out;
}

Bytes decompress_lzss(const std::uint8_t* comp, std::size_t comp_len,
                      std::uint32_t decomp_size) {
    Bytes out(decomp_size, 0);
    // Signed indices throughout: `src_idx + k` legitimately goes negative when
    // a back-reference points before the start of the window, and the explicit
    // `0 <= src_idx + k` test prevents unsigned wraparound.
    const std::int64_t limit = static_cast<std::int64_t>(decomp_size);
    const std::int64_t clen = static_cast<std::int64_t>(comp_len);
    std::int64_t si = 0;
    std::int64_t di = 0;

    while (di < limit) {
        if (si >= clen) {
            break;
        }
        std::uint8_t flag = comp[si];
        ++si;
        for (int bit = 0; bit < kBitsPerFlagByte; ++bit) {
            if (di >= limit) {
                out.resize(static_cast<std::size_t>(di));
                return out;
            }
            if ((flag & kLiteralBitMask) != 0) {
                if (si >= clen) {
                    out.resize(static_cast<std::size_t>(di));
                    return out;
                }
                out[static_cast<std::size_t>(di)] = comp[si];
                ++si;
                ++di;
                flag = static_cast<std::uint8_t>(flag >> kFlagBitShift);
                continue;
            }
            if (si + 1 >= clen) {
                out.resize(static_cast<std::size_t>(di));
                return out;
            }
            const std::uint32_t word =
                static_cast<std::uint32_t>(comp[si]) |
                (static_cast<std::uint32_t>(comp[si + 1]) << kByteShift8);
            si += kBackrefPairBytes;
            const std::int64_t length =
                static_cast<std::int64_t>(word & kBackrefLenMask) + kBackrefMinLength;
            const std::int64_t offset = static_cast<std::int64_t>(word >> kBackrefOffsetShift);
            const std::int64_t src_idx = di - offset;
            for (std::int64_t k = 0; k < length; ++k) {
                if (di >= limit) {
                    out.resize(static_cast<std::size_t>(di));
                    return out;
                }
                const std::int64_t s = src_idx + k;
                out[static_cast<std::size_t>(di)] =
                    (s >= 0 && s < di) ? out[static_cast<std::size_t>(s)] : 0;
                ++di;
            }
            flag = static_cast<std::uint8_t>(flag >> kFlagBitShift);
        }
    }
    out.resize(static_cast<std::size_t>(di));
    return out;
}

Bytes decode_scene_chunk(const Bytes& scene_bytes, const std::uint8_t* private_key) {
    Bytes data = scene_bytes;
    if (private_key != nullptr) {
        for (std::size_t i = 0; i < data.size(); ++i) {
            data[i] = static_cast<std::uint8_t>(data[i] ^ private_key[i & kPrivateKeyMask]);
        }
    }
    data = decrypt_universal(data);
    if (data.size() < kChunkHeaderBytes) {
        throw std::runtime_error("scene chunk too short (" + std::to_string(data.size()) +
                                 " bytes)");
    }
    const std::uint32_t comp_size = read_u32_le(data, 0);
    const std::uint32_t decomp_size = read_u32_le(data, kChunkDecompOffset);
    if (decomp_size == 0 || decomp_size > kMaxPlausibleDecompBytes) {
        char buf[kErrorBufSize];
        std::snprintf(buf, sizeof(buf), "implausible decomp_size 0x%X (comp_size=0x%X)",
                      decomp_size, comp_size);
        throw std::runtime_error(buf);
    }
    return decompress_lzss(data.data() + kChunkHeaderBytes,
                           data.size() - kChunkHeaderBytes,
                           decomp_size);
}

std::vector<std::string> parse_scene_strings(const Bytes& d) {
    std::vector<RawEntry> raw;
    if (!read_raw_entries(d, raw)) {
        return {};
    }

    std::vector<std::string> out;
    out.reserve(raw.size());
    std::vector<std::uint16_t> units;
    for (std::size_t j = 0; j < raw.size(); ++j) {
        if (!raw[j].present) {
            out.emplace_back();
            continue;
        }
        // SiglusEngine.exe (Scene::get_str at RVA 0x14C8E0) unconditionally XORs
        // every string slot `j` in every scene with `(j * 0x7087) & 0xFFFF`.
        // At `j == 0` the key is `0x0000` (identity), while `units_to_utf8_direct`
        // enforces surrogate-free BMP code units across all slots uniformly.
        const std::size_t n = raw[j].len / kBytesPerUtf16Unit;
        const std::uint16_t key =
            static_cast<std::uint16_t>((j * kSsV2Multiplier) & kSsV2KeyMask);
        units.assign(n, 0);
        for (std::size_t u = 0; u < n; ++u) {
            units[u] = static_cast<std::uint16_t>(
                read_unit_le(d, raw[j].off + u * kBytesPerUtf16Unit) ^ key);
        }
        std::string text = units_to_utf8_direct(units.data(), n);
        text = trim_right_chars(text, std::string(1, '\0'));
        text = trim_right(text);
        out.push_back(std::move(text));
    }
    return out;
}

std::vector<std::string> decode_scene(const Bytes& scene_bytes) {
    Bytes decompressed;
    try {
        decompressed = decode_scene_chunk(scene_bytes, ADIEU_PRIVATE_KEY.data());
    } catch (const std::exception&) {
        return {};
    }
    return parse_scene_strings(decompressed);
}

const std::array<std::uint8_t, kSiglusKey256Size>& warm_global_key() {
    return SIGLUS_KEY_256;
}

}  // namespace adi::siglus
