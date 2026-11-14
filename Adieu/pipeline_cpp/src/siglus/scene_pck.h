// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Scene.pck container walk: the 0x5C-byte header, the macro/scene name tables
// and the per-scene chunk index.
//
// Pure parsing -- the crypto and the string pool live in siglus_decoder.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "common/util.h"

namespace adi::siglus {

constexpr std::uint32_t HEADER_SIZE = 0x5C;
constexpr int SECTION_COUNT = 11;

// Section indices.
constexpr int SEC_MACRO_NAME_IDX = 4;
constexpr int SEC_MACRO_NAME_BLOB = 5;
constexpr int SEC_SCRIPT_NAME_IDX = 6;
constexpr int SEC_SCRIPT_NAME_BLOB = 7;
constexpr int SEC_SCRIPT_DATA_IDX = 8;
// The scene-data blob starts at sec[9].OFFSET, not sec[8]'s -- section 9 is
// nominally the per-script key table, but its offset is where the chunks
// begin (verified at runtime by the proxy DLL's key dumper).
constexpr int SEC_SCRIPT_KEY = 9;

struct Section {
    std::uint32_t offset = 0;
    std::uint32_t count = 0;
};

struct Header {
    std::uint32_t header_size = 0;
    std::array<Section, SECTION_COUNT> sections{};
};

struct ScriptMeta {
    std::string name;
    std::uint32_t rel_offset = 0;
    std::uint64_t abs_offset = 0;
    std::uint32_t size = 0;
    std::string key_hex;  // always "" -- Adieu has no per-script key
};

Header parse_header(const Bytes& data);

// UTF-16LE name table: index entries are (wchar_offset, wchar_length) into the
// blob.  Out-of-range slices are CLAMPED, not rejected -- an index entry that
// overruns the blob must still yield a (possibly short) name rather than abort
// the walk, and a clamp to an odd byte count yields a trailing U+FFFD.
std::vector<std::string> read_name_table(const Bytes& data, std::uint32_t idx_off,
                                         std::uint32_t idx_cnt, std::uint32_t blob_off);

std::vector<ScriptMeta> read_script_meta(const Bytes& data, const Header& h,
                                         const std::vector<std::string>& script_names);

// Bytes [start, start+size) clamped to the buffer; a start past the end
// yields empty rather than throwing.
Bytes slice_clamped(const Bytes& data, std::uint64_t start, std::uint64_t size);

}  // namespace adi::siglus
