// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// "AMK2MENU" v1 container test serializer for synthetic fixture construction.

#include "menu_patch_format.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace menu_patch_test {

using Bytes = std::vector<std::uint8_t>;

inline constexpr std::uint8_t kSpecFlagBackupStandalone = 0x01;
inline constexpr std::size_t kSpecHeaderReservedBytes = 12;
inline constexpr std::size_t kSpecRecordReservedBytes = 3;
inline constexpr std::uint32_t kSpecFormatVersion = 1;
inline constexpr std::uint32_t kSpecKeyStreamMul = 0x9Eu;
inline constexpr std::uint32_t kSpecKeyStreamAdd = 0x37u;
inline constexpr std::uint32_t kSpecByteMask = 0xFFu;
inline constexpr std::uint32_t kSpecHashLaneMask = 3u;
inline constexpr unsigned kSpecBitsPerByte = 8;
inline constexpr std::size_t kSpecU8 = 1;
inline constexpr std::size_t kSpecU16 = 2;
inline constexpr std::size_t kSpecU32 = 4;
inline constexpr std::size_t kSpecU64 = 8;

inline void PutLe(Bytes& out, std::uint64_t value, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) {
        out.push_back(static_cast<std::uint8_t>(
            (value >> (i * kSpecBitsPerByte)) & kSpecByteMask));
    }
}

inline void PutString(Bytes& out, const std::string& s) {
    PutLe(out, s.size(), kSpecU16);
    out.insert(out.end(), s.begin(), s.end());
}

struct SpecEntry {
    std::string source;
    std::string target;
    std::uint8_t flags = 0;
    std::uint64_t orig_offset = 0;
    Bytes orig_slice;
    Bytes new_raw;
};

inline Bytes MaskPayload(const SpecEntry& entry, std::uint32_t orig_hash) {
    Bytes payload(entry.new_raw.size());
    for (std::size_t k = 0; k < payload.size(); ++k) {
        const auto k32 = static_cast<std::uint32_t>(k);
        const auto hash_byte = static_cast<std::uint8_t>(
            (orig_hash >> ((k32 & kSpecHashLaneMask) * kSpecBitsPerByte)) &
            kSpecByteMask);
        const auto stream_byte = static_cast<std::uint8_t>(
            (k32 * kSpecKeyStreamMul + kSpecKeyStreamAdd) & kSpecByteMask);
        payload[k] = static_cast<std::uint8_t>(
            entry.new_raw[k] ^ entry.orig_slice[k % entry.orig_slice.size()] ^
            hash_byte ^ stream_byte);
    }
    return payload;
}

inline Bytes EncodeContainer(const std::vector<SpecEntry>& entries) {
    Bytes descriptors;
    Bytes records;
    for (const SpecEntry& e : entries) {
        const std::uint32_t orig_hash =
            menu_patch::MurmurHash2(e.orig_slice.data(), e.orig_slice.size());
        const std::uint32_t new_hash =
            menu_patch::MurmurHash2(e.new_raw.data(), e.new_raw.size());

        Bytes descriptor;
        PutString(descriptor, e.source);
        PutString(descriptor, e.target);
        PutLe(descriptor, e.flags, kSpecU8);
        PutLe(descriptor, e.orig_offset, kSpecU64);
        PutLe(descriptor, e.orig_slice.size(), kSpecU32);
        PutLe(descriptor, orig_hash, kSpecU32);
        PutLe(descriptor, e.new_raw.size(), kSpecU32);
        PutLe(descriptor, new_hash, kSpecU32);
        descriptors.insert(descriptors.end(), descriptor.begin(),
                           descriptor.end());

        PutString(records, e.source);
        PutString(records, e.target);
        PutLe(records, e.flags, kSpecU8);
        records.insert(records.end(), kSpecRecordReservedBytes, 0);
        PutLe(records, e.orig_offset, kSpecU64);
        PutLe(records, e.orig_slice.size(), kSpecU32);
        PutLe(records, orig_hash, kSpecU32);
        PutLe(records, e.new_raw.size(), kSpecU32);
        PutLe(records, new_hash, kSpecU32);
        const Bytes payload = MaskPayload(e, orig_hash);
        records.insert(records.end(), payload.begin(), payload.end());
    }

    Bytes out = {'A', 'M', 'K', '2', 'M', 'E', 'N', 'U'};
    PutLe(out, kSpecFormatVersion, kSpecU32);
    PutLe(out, entries.size(), kSpecU32);
    PutLe(out, menu_patch::MurmurHash2(descriptors.data(), descriptors.size()),
          kSpecU32);
    out.insert(out.end(), kSpecHeaderReservedBytes, 0);
    out.insert(out.end(), records.begin(), records.end());
    return out;
}

inline Bytes PseudoRandomBytes(std::size_t size, std::uint32_t seed) {
    constexpr unsigned kXorShiftA = 13;
    constexpr unsigned kXorShiftB = 17;
    constexpr unsigned kXorShiftC = 5;
    constexpr std::uint32_t kDefaultNonZeroSeed = 1u;
    Bytes out(size);
    std::uint32_t state = seed == 0 ? kDefaultNonZeroSeed : seed;
    for (std::uint8_t& b : out) {
        state ^= state << kXorShiftA;
        state ^= state >> kXorShiftB;
        state ^= state << kXorShiftC;
        b = static_cast<std::uint8_t>(state & kSpecByteMask);
    }
    return out;
}

}  // namespace menu_patch_test
