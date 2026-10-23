// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Artemis Engine PF8 archive reader.
//
// Format
// ======
//     [3]   "pf8" magic
//     [4]   uint32 LE index_size
//     [N]   index data (index_size bytes, plaintext)
//     [...] file data, absolute byte offsets from start of file
//
// Index layout (parsed left-to-right):
//     [4]   uint32 LE entry_count
//     For each entry:
//         [4] uint32 LE name_len
//         [N] CP932 path (uses '\' separators)
//         [4] uint32 LE reserved (== 0)
//         [4] uint32 LE file_offset (absolute)
//         [4] uint32 LE file_size
//     [..]  extension cache trailer (ignored -- we only need readers)
//
// Encryption
// ==========
// Every file payload (script AND media: .ast, .lua, .iet, .png, .ogg, .ttf,
// ...) is XOR-encrypted with a 20-byte key:
//
//     key = SHA-1(index_bytes)
//
// applied modulo 20.  Each .pfs has its own key (different index).
// Reference: GARbro/ArcFormats/Artemis/ArcPFS.cs (`OpenPf` for v6/v8).
//
// Multiple .pfs files exist (sakuranotoki.pfs, .pfs.000, .001, .002, .010).
// Higher-numbered patch archives override matching paths in the base archive
// (engine load order).
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "common/util.h"

namespace snt::pfs {

constexpr std::size_t KEY_LEN = 20;
using Key = std::array<std::uint8_t, KEY_LEN>;

struct Entry {
    std::string name;  // CP932 path from the archive, decoded to UTF-8
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
};

// Raw index bytes + total file size.  Throws on a bad magic / short read.
Bytes read_index(const std::string& path, std::uint64_t* file_total = nullptr);

// SHA-1 of the index bytes.
Key archive_key(const std::string& path);

// Parsed entry table.
std::vector<Entry> list_pfs(const std::string& path,
                            std::uint64_t* file_total = nullptr);

// XOR `data` in place with `key`, cycling from `start_offset % KEY_LEN`.
void xor_decrypt(Bytes& data, const Key& key, std::size_t start_offset = 0);

// Read + decrypt one entry by index.
Bytes read_entry(const std::string& path, std::size_t idx, const Key& key);

// name -> (archive path, index within that archive), later archives winning,
// plus the per-archive key.
struct Overlay {
    // std::map so iteration order is deterministic and already sorted by name:
    // every consumer wants name order, and step 1 relies on it to keep
    // extracted_text.json stable across runs.
    std::map<std::string, std::pair<std::string, std::size_t>> entries;
    std::map<std::string, Key> keys;
    // Archive paths in engine load order, for logging.
    std::vector<std::string> archives;
};

Overlay build_overlay(const std::string& game_dir,
                      const std::string& base_name = "sakuranotoki");

}  // namespace snt::pfs
