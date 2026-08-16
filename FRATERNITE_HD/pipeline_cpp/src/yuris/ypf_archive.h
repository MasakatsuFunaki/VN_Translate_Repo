// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// YU-RIS YPF v490 container reader + YSTB script decryptor.
//
// YPF header: magic 'YPF\0', version, file_count, index_size, 16B reserved.
// Index entries: name_hash(4) + name_size^0xFF(1) + name^0xFF(N) + type(1) +
// compressed(1) + raw_size(4) + packed_size(4) + data_offset(8) +
// data_hash(4) -- 27 + N bytes.  The 1-byte name_size key varies per entry,
// so the real name length is found by probing for a valid footer.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/util.h"

namespace frat::yuris {

// YSTB bodies are XOR-encrypted per u32 with a single archive-wide ScriptKey.
constexpr std::uint32_t YSTB_SCRIPT_KEY = 0x6594DAC3u;
// v490 encrypts index filenames byte-wise with 0xFF.
constexpr std::uint8_t YPF_NAME_XOR_KEY = 0xFFu;

struct YpfEntry {
    std::string name;
    int type = 0;
    int compressed = 0;
    std::uint32_t raw_size = 0;
    std::uint32_t packed_size = 0;
    std::uint64_t data_offset = 0;
};

// Throws std::runtime_error("not a YPF: <path>") when the magic is wrong.
std::vector<YpfEntry> parse_ypf_index(const std::string& path);

// Same, over an already-loaded archive image (the whole file is kept in
// memory: it is an 11 MB-class file and every entry is read from it).
std::vector<YpfEntry> parse_ypf_index_bytes(const Bytes& file, const std::string& path);

// No-op unless the blob is a YSTB whose header section sizes add up.
Bytes decrypt_ystb(const Bytes& blob);

// zlib-inflate when compressed, then decrypt_ystb when the result is a YSTB.
Bytes read_entry(const Bytes& file, const YpfEntry& entry);

// Archive names look like "ysbin\\yst00156.ybn"; rsplit on '\\' then '/'.
std::string basename(const std::string& entry_name);

}  // namespace frat::yuris
