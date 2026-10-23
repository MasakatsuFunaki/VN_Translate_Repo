// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Synthetic PF8 archives for tests that need an archive on disk without
// touching game data.
//
// make_pf8 is the inverse of what artemis/pfs_archive.cpp reads: it lays the
// index out twice, because a payload offset depends on the size of the index
// that records it, then derives the XOR key the same way the reader does --
// SHA-1 of the index bytes.  XOR is its own inverse, so encrypting a payload
// and decrypting it are the same call.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <openssl/sha.h>

#include "artemis/pfs_archive.h"
#include "common/util.h"

namespace snt::test_fixture {

inline void put_u32(Bytes& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
    b.push_back(static_cast<std::uint8_t>(v >> 16));
    b.push_back(static_cast<std::uint8_t>(v >> 24));
}

// A PF8 archive holding the given (name, payload) pairs, in that order.
inline Bytes make_pf8(const std::vector<std::pair<std::string, Bytes>>& files) {
    auto build_index = [&](std::uint32_t data_start) {
        Bytes index;
        put_u32(index, static_cast<std::uint32_t>(files.size()));
        std::uint32_t off = data_start;
        for (const auto& [name, payload] : files) {
            put_u32(index, static_cast<std::uint32_t>(name.size()));
            index.insert(index.end(), name.begin(), name.end());
            put_u32(index, 0);  // reserved
            put_u32(index, off);
            put_u32(index, static_cast<std::uint32_t>(payload.size()));
            off += static_cast<std::uint32_t>(payload.size());
        }
        return index;
    };

    const std::uint32_t index_size = static_cast<std::uint32_t>(build_index(0).size());
    const std::uint32_t data_start = 7 + index_size;
    Bytes index = build_index(data_start);

    pfs::Key key{};
    SHA1(index.data(), index.size(), key.data());

    Bytes out;
    out.insert(out.end(), {'p', 'f', '8'});
    put_u32(out, index_size);
    out.insert(out.end(), index.begin(), index.end());
    for (const auto& [name, payload] : files) {
        Bytes enc = payload;
        pfs::xor_decrypt(enc, key, 0);
        out.insert(out.end(), enc.begin(), enc.end());
    }
    return out;
}

inline std::string temp_path(const std::string& name) {
    return (std::filesystem::temp_directory_path() / name).u8string();
}

}  // namespace snt::test_fixture
