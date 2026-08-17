// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// YU-RIS YPF v490 container reader + YSTB script decryptor.
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

std::vector<YpfEntry> parse_ypf_index(const std::string& path);
std::vector<YpfEntry> parse_ypf_index_bytes(const Bytes& file, const std::string& path);
Bytes decrypt_ystb(const Bytes& blob);
Bytes read_entry(const Bytes& file, const YpfEntry& entry);
std::string basename(const std::string& entry_name);

}  // namespace frat::yuris
