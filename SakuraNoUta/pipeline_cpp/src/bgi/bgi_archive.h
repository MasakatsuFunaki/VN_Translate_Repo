// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// BGI / Buriko archive helpers: ARC20 flat archive and DSC FORMAT 1.00
// (Huffman + LZ) decompression.  Ref: asmodean, GARbro, arc_unpacker.
#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "common/util.h"

namespace snu::bgi {

struct ArcEntry {
    std::string name;      // UTF-8 (decoded from CP932, errors='replace')
    std::uint32_t offset;  // relative to the archive data section
    std::uint32_t size;
};

// Decompress a DSC FORMAT 1.00 blob.  Throws std::runtime_error on bad
// magic or malformed Huffman code lengths.
Bytes dsc_decompress(const Bytes& data);

inline bool is_dsc(const Bytes& raw) {
    static const char magic[] = "DSC FORMAT 1.00";
    return raw.size() >= 16 && std::memcmp(raw.data(), magic, 16) == 0;
}

// List every entry in an ARC20 archive.  Throws on bad magic.
std::vector<ArcEntry> list_arc(const std::string& path);

// Read one entry by index; transparently DSC-decompresses when
// `decompress` and the payload carries the DSC magic.
Bytes read_entry(const std::string& path, std::size_t idx, bool decompress = true);
// Read one entry by name.  Throws std::out_of_range when absent.
Bytes read_entry_by_name(const std::string& path, const std::string& name,
                         bool decompress = true);

// Extract every entry into out_dir.  Returns the entry count.
std::size_t extract_all(const std::string& arc_path, const std::string& out_dir,
                        bool decompress = true);

// Rebuild an ARC20 archive, substituting entries named in `replacements`.
void repack_arc(const std::string& in_path, const std::string& out_path,
                const std::map<std::string, Bytes>& replacements);

}  // namespace snu::bgi
