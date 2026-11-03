// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "yuris/ypf_archive.h"

#include "yuris/cp932_scan.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include <zlib.h>

namespace frat::yuris {

namespace {

constexpr std::size_t kMagicSize = 4u;
constexpr std::size_t kFileCountOffset = 8u;
constexpr std::size_t kIdxSizeOffset = 12u;
constexpr std::size_t kNameSizeRawOffset = 4u;
constexpr std::size_t kFooterCompressedOffset = 1u;
constexpr std::size_t kFooterRawSizeOffset = 2u;
constexpr std::size_t kFooterPackedSizeOffset = 6u;
constexpr std::size_t kFooterDataOffsetOffset = 10u;
constexpr std::size_t kFooterDataHashOffset = 18u;

constexpr std::uint8_t kMaxEntryType = 20u;
constexpr std::uint8_t kMaxCompressedFlag = 1u;
constexpr std::uint8_t kAsciiPrintMin = 0x20u;
constexpr std::uint8_t kAsciiPrintMax = 0x7Eu;
constexpr std::uint8_t kHalfwidthKanaMin = 0xA1u;
constexpr std::uint8_t kHalfwidthKanaMax = 0xDFu;
constexpr std::uint8_t kExtensionDotByte = 0x2Eu;
constexpr long long kHintLookaheadMargin = 8;
constexpr long long kMaxProbedNameLength = 255;

constexpr std::uint32_t kMurmurMultiplier = 0x5BD1E995u;
constexpr int kMurmurShiftR = 24;
constexpr int kMurmurFinalShift1 = 13;
constexpr int kMurmurFinalShift2 = 15;
constexpr std::size_t kWordBytes = 4u;
constexpr std::size_t kU64Bytes = 8u;
constexpr std::size_t kDoubleByteCharLen = 2u;
constexpr std::size_t kRemThreeBytes = 3u;
constexpr std::size_t kRemTwoBytes = 2u;
constexpr std::size_t kRemOneByte = 1u;
constexpr std::size_t kByteOffset1 = 1u;
constexpr std::size_t kByteOffset2 = 2u;
constexpr std::size_t kByteOffset3 = 3u;
constexpr int kBitsPerByte = 8;
constexpr int kShiftByte1 = 8;
constexpr int kShiftByte2 = 16;
constexpr int kShiftByte3 = 24;

constexpr std::size_t kYstbHeaderSize = 0x20u;
constexpr std::size_t kYstbCodeSizeOffset = 12u;
constexpr std::size_t kYstbArgSizeOffset = 16u;
constexpr std::size_t kYstbResSizeOffset = 20u;
constexpr std::size_t kYstbOffSizeOffset = 24u;
constexpr std::uint32_t kLowByteMask = 0xFFu;
constexpr std::size_t kInflateMinFactor = 4u;
constexpr std::size_t kInflateBasePad = 64u;
constexpr std::size_t kInflateGrowMultiplier = 2u;
constexpr std::size_t kInflateGrowPad = 1024u;

std::uint32_t rd_u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[kByteOffset1]) << kShiftByte1) |
           (static_cast<std::uint32_t>(p[kByteOffset2]) << kShiftByte2) |
           (static_cast<std::uint32_t>(p[kByteOffset3]) << kShiftByte3);
}

std::uint32_t rd_u32(const Bytes& b, std::size_t off) {
    return rd_u32(b.data() + off);
}

std::uint64_t rd_u64(const Bytes& b, std::size_t off) {
    std::uint64_t v = 0;
    for (std::size_t i = kU64Bytes; i > 0; --i) {
        v = (v << kBitsPerByte) | b[off + i - 1];
    }
    return v;
}

bool is_valid_cp932_entry_name(const Bytes& idx, std::size_t off, std::size_t len,
                               bool require_dot) {
    std::size_t j = 0;
    bool has_dot = false;
    while (j < len) {
        const std::uint8_t d = static_cast<std::uint8_t>(idx[off + j] ^ YPF_NAME_XOR_KEY);
        if (d == kExtensionDotByte) {
            has_dot = true;
        }
        if ((d >= kAsciiPrintMin && d <= kAsciiPrintMax) ||
            (d >= kHalfwidthKanaMin && d <= kHalfwidthKanaMax)) {
            ++j;
            continue;
        }
        if (is_cp932_lead(d)) {
            if (j + kDoubleByteCharLen > len) {
                return false;
            }
            const std::uint8_t trail =
                static_cast<std::uint8_t>(idx[off + j + kByteOffset1] ^ YPF_NAME_XOR_KEY);
            if (!is_cp932_trail(trail)) {
                return false;
            }
            j += kDoubleByteCharLen;
            continue;
        }
        return false;
    }
    return !require_dot || has_dot;
}

bool is_valid_ascii_entry_name(const Bytes& idx, std::size_t off, std::size_t len) {
    for (std::size_t j = 0; j < len; ++j) {
        const std::uint8_t d = static_cast<std::uint8_t>(idx[off + j] ^ YPF_NAME_XOR_KEY);
        if (d < kAsciiPrintMin || d > kAsciiPrintMax) {
            return false;
        }
    }
    return true;
}

bool is_valid_entry_footer(const Bytes& idx, std::size_t tail) {
    if (tail + YPF_ENTRY_FOOTER_SIZE > idx.size()) {
        return false;
    }
    const std::uint8_t t = idx[tail];
    const std::uint8_t c = idx[tail + kFooterCompressedOffset];
    if (t > kMaxEntryType || c > kMaxCompressedFlag) {
        return false;
    }
    const std::uint32_t raw = rd_u32(idx, tail + kFooterRawSizeOffset);
    const std::uint32_t packed = rd_u32(idx, tail + kFooterPackedSizeOffset);
    return (c == 0 && raw == packed) || (c == 1 && raw > 0);
}

int probe_entry_name_length(const Bytes& idx, std::size_t off, int hinted) {
    const long long room = static_cast<long long>(idx.size()) -
                           static_cast<long long>(off) -
                           static_cast<long long>(YPF_ENTRY_FOOTER_SIZE);
    if (room <= 0) {
        return 0;
    }
    const long long max_cp932_try = std::min<long long>(
        std::max<long long>(hinted + kHintLookaheadMargin, kMaxProbedNameLength), room);
    for (long long n = 1; n <= max_cp932_try; ++n) {
        const std::size_t len = static_cast<std::size_t>(n);
        if (!is_valid_entry_footer(idx, off + len)) {
            continue;
        }
        if (is_valid_cp932_entry_name(idx, off, len, true)) {
            return static_cast<int>(n);
        }
    }
    const long long max_ascii_try = std::min<long long>(hinted + kHintLookaheadMargin, room);
    for (long long n = 1; n <= max_ascii_try; ++n) {
        const std::size_t len = static_cast<std::size_t>(n);
        if (!is_valid_entry_footer(idx, off + len)) {
            continue;
        }
        if (is_valid_ascii_entry_name(idx, off, len)) {
            return static_cast<int>(n);
        }
    }
    return 0;
}

// raw_size is the index's claim; grow the buffer if it lies.
Bytes zlib_inflate(const Bytes& in, std::uint32_t hint) {
    Bytes out(std::max<std::size_t>(hint, in.size() * kInflateMinFactor + kInflateBasePad));
    for (;;) {
        uLongf dest_len = static_cast<uLongf>(out.size());
        const int rc = ::uncompress(out.data(), &dest_len, in.data(),
                                    static_cast<uLong>(in.size()));
        if (rc == Z_OK) {
            out.resize(dest_len);
            return out;
        }
        if (rc != Z_BUF_ERROR) {
            throw std::runtime_error("zlib inflate failed");
        }
        out.resize(out.size() * kInflateGrowMultiplier + kInflateGrowPad);
    }
}

}  // namespace

std::uint32_t ypf_murmur_hash2(const std::uint8_t* data, std::size_t len,
                               std::uint32_t seed) {
    std::uint32_t h = seed ^ static_cast<std::uint32_t>(len);
    std::size_t i = 0;
    while (i + kWordBytes <= len) {
        std::uint32_t k = rd_u32(data + i);
        k *= kMurmurMultiplier;
        k ^= k >> kMurmurShiftR;
        k *= kMurmurMultiplier;
        h *= kMurmurMultiplier;
        h ^= k;
        i += kWordBytes;
    }
    const std::size_t rem = len - i;
    if (rem == kRemThreeBytes) {
        h ^= static_cast<std::uint32_t>(data[i + kByteOffset2]) << kShiftByte2;
    }
    if (rem >= kRemTwoBytes) {
        h ^= static_cast<std::uint32_t>(data[i + kByteOffset1]) << kShiftByte1;
    }
    if (rem >= kRemOneByte) {
        h ^= static_cast<std::uint32_t>(data[i]);
        h *= kMurmurMultiplier;
    }
    h ^= h >> kMurmurFinalShift1;
    h *= kMurmurMultiplier;
    h ^= h >> kMurmurFinalShift2;
    return h;
}

std::string basename(const std::string& entry_name) {
    std::string s = entry_name;
    if (const std::size_t p = s.rfind('\\'); p != std::string::npos) {
        s = s.substr(p + 1);
    }
    if (const std::size_t p = s.rfind('/'); p != std::string::npos) {
        s = s.substr(p + 1);
    }
    return s;
}

std::vector<YpfEntry> parse_ypf_index_bytes(const Bytes& file, const std::string& path) {
    if (file.size() < YPF_HEADER_SIZE || std::memcmp(file.data(), "YPF\0", kMagicSize) != 0) {
        throw std::runtime_error("not a YPF: " + path);
    }
    const std::uint32_t file_count = rd_u32(file, kFileCountOffset);
    const std::uint32_t idx_size = rd_u32(file, kIdxSizeOffset);

    const std::size_t idx_begin = YPF_HEADER_SIZE;
    const std::size_t idx_end = std::min<std::size_t>(file.size(), idx_begin + idx_size);
    const Bytes idx(file.begin() + static_cast<std::ptrdiff_t>(idx_begin),
                    file.begin() + static_cast<std::ptrdiff_t>(idx_end));

    std::vector<YpfEntry> entries;
    entries.reserve(file_count);
    std::size_t off = 0;
    for (std::uint32_t k = 0; k < file_count; ++k) {
        if (off + YPF_ENTRY_PREFIX_SIZE > idx.size()) {
            break;
        }
        const std::uint32_t name_hash = rd_u32(idx, off);
        const std::uint8_t name_size_raw = idx[off + kNameSizeRawOffset];
        off += YPF_ENTRY_PREFIX_SIZE;

        const int hinted = name_size_raw ^ YPF_NAME_XOR_KEY;
        int name_len = probe_entry_name_length(idx, off, hinted);
        if (name_len == 0) {
            name_len = hinted;
        }
        if (off + static_cast<std::size_t>(name_len) + YPF_ENTRY_FOOTER_SIZE > idx.size()) {
            break;
        }

        const std::size_t nlen = static_cast<std::size_t>(name_len);
        Bytes name_bytes_xor(idx.begin() + static_cast<std::ptrdiff_t>(off),
                             idx.begin() + static_cast<std::ptrdiff_t>(off + nlen));
        Bytes name_bytes(nlen);
        for (std::size_t j = 0; j < nlen; ++j) {
            name_bytes[j] = static_cast<std::uint8_t>(name_bytes_xor[j] ^ YPF_NAME_XOR_KEY);
        }

        YpfEntry e;
        e.name = cp932_to_utf8_replace(name_bytes.data(), name_bytes.size());
        e.name_hash = name_hash;
        e.name_size_raw = name_size_raw;
        e.name_bytes_xor = std::move(name_bytes_xor);
        off += nlen;

        e.type = idx[off];
        e.compressed = idx[off + kFooterCompressedOffset];
        e.raw_size = rd_u32(idx, off + kFooterRawSizeOffset);
        e.packed_size = rd_u32(idx, off + kFooterPackedSizeOffset);
        e.data_offset = rd_u64(idx, off + kFooterDataOffsetOffset);
        e.data_hash = rd_u32(idx, off + kFooterDataHashOffset);
        off += YPF_ENTRY_FOOTER_SIZE;
        entries.push_back(std::move(e));
    }
    return entries;
}

std::vector<YpfEntry> parse_ypf_index(const std::string& path) {
    return parse_ypf_index_bytes(read_file(path), path);
}

Bytes decrypt_ystb(const Bytes& blob) {
    if (blob.size() <= kYstbHeaderSize || std::memcmp(blob.data(), "YSTB", kMagicSize) != 0) {
        return blob;
    }
    const std::uint32_t csz = rd_u32(blob, kYstbCodeSizeOffset);
    const std::uint32_t asz = rd_u32(blob, kYstbArgSizeOffset);
    const std::uint32_t rsz = rd_u32(blob, kYstbResSizeOffset);
    const std::uint32_t osz = rd_u32(blob, kYstbOffSizeOffset);
    // Malformed or a different YSTB variant: leave it alone.
    if (static_cast<std::uint64_t>(kYstbHeaderSize) + csz + asz + rsz + osz != blob.size()) {
        return blob;
    }

    Bytes buf = blob;
    std::size_t off = kYstbHeaderSize;
    for (const std::uint32_t sz : {csz, asz, rsz, osz}) {
        const std::size_t end4 = off + (sz & ~static_cast<std::uint32_t>(kWordBytes - 1));
        for (std::size_t i = off; i < end4; i += kWordBytes) {
            const std::uint32_t w = rd_u32(buf, i) ^ YSTB_SCRIPT_KEY;
            buf[i] = static_cast<std::uint8_t>(w);
            buf[i + 1] = static_cast<std::uint8_t>(w >> kShiftByte1);
            buf[i + 2] = static_cast<std::uint8_t>(w >> kShiftByte2);
            buf[i + 3] = static_cast<std::uint8_t>(w >> kShiftByte3);
        }
        const std::uint32_t rem = sz & static_cast<std::uint32_t>(kWordBytes - 1);
        std::uint32_t k = YSTB_SCRIPT_KEY;
        for (std::uint32_t j = 0; j < rem; ++j) {
            buf[end4 + j] ^= static_cast<std::uint8_t>(k & kLowByteMask);
            k >>= kBitsPerByte;
        }
        off += sz;
    }
    return buf;
}

Bytes read_entry(const Bytes& file, const YpfEntry& entry) {
    Bytes blob;
    if (entry.data_offset < file.size()) {
        const std::size_t begin = static_cast<std::size_t>(entry.data_offset);
        const std::size_t end = std::min<std::size_t>(file.size(), begin + entry.packed_size);
        blob.assign(file.begin() + static_cast<std::ptrdiff_t>(begin),
                    file.begin() + static_cast<std::ptrdiff_t>(end));
    }
    if (entry.compressed) {
        blob = zlib_inflate(blob, entry.raw_size);
    }
    if (blob.size() >= kMagicSize && std::memcmp(blob.data(), "YSTB", kMagicSize) == 0) {
        blob = decrypt_ystb(blob);
    }
    return blob;
}

}  // namespace frat::yuris
