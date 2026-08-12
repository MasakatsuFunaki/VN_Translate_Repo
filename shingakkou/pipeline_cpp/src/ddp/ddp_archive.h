// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// DDSystem DDP2 / DDP3 containers.
//
// Two halves:
//
//   * the sin_text.dat reader -- index walk, ShsCompression (LZ77)
//     decompression and the DDWuHXB XOR cipher, all derived from GARbro's
//     SHSystem/ArcHXP.cs.
//
//   * the CG-archive repacker, used by 04_find_narrative_cg to substitute
//     patched BMPs into sin_cgev.dat / sin_sysd.dat.  Extraction of those
//     archives is NOT here: it shells out to TOOLS/garbro/extract_ddp.exe,
//     because the image codecs live in GARbro.
//
// DDP2 layout:
//   0x00  "DDP2" + count(u32) + padding[24]        = 0x20-byte header
//   0x20  count * 16-byte index: offset, unpacked, packed, padding
//   data immediately after the index
//
// DDP3 layout:
//   0x00  "DDP3" + count(u32) + dataStart(u32) + padding[20]
//         count is the number of SECTIONS, not entries
//   0x20  count * 8-byte section table: block size, absolute offset
//   at each offset: variable-length entries filling `block size`
//         entry: size(u8), offset(u32), unpacked(u32), packed(u32),
//                padding(u32), name as UTF-16LE (size-17 bytes)
//   zero padding up to dataStart, then the image data
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/util.h"

namespace shin::ddp {

struct ScriptEntry {
    std::string name;           // UTF-16LE -> UTF-8, trailing U+0000 stripped
    std::uint32_t offset;       // absolute file offset of the compressed payload
    std::uint32_t decomp_size;
    std::uint32_t comp_size;
};

// Walks the 32-entry section table at offset 32 and every section's inline
// entry list.  Entries come back in traversal order (section 0..N, then entry
// order within the section) -- NOT sorted by file offset.  That order becomes
// the top-level key order of extracted_text.json, so it is load-bearing.
//
// Throws std::runtime_error when the magic is not "DDP3".
std::vector<ScriptEntry> parse_ddp3(const Bytes& data);

// ShsCompression LZ77.  Back-references MUST be copied byte by byte forward:
// 1,345 of them in sin_text.dat read bytes the same copy just wrote, so
// memcpy is UB here and memmove produces different (wrong) output.
Bytes shs_decompress(const Bytes& src, std::uint32_t decomp_size);

// XOR cipher over everything from offset 0x10, keyed off the BE24 plaintext
// length at offset 8.  The multiply wraps at 32 bits -- widening it changes
// the key.  Inputs shorter than 0x11 bytes come back unchanged.
Bytes decrypt_hxb(const Bytes& data);

// ---------------------------------------------------------------------------
// CG-archive repacker
// ---------------------------------------------------------------------------

// One entry as list_archive() reports it.
struct ArchiveEntry {
    std::string name;
    std::uint32_t offset = 0;
    std::uint32_t unpacked = 0;
    std::uint32_t packed = 0;
};

// Replacement payloads keyed by entry name, in insertion order.  Insertion
// order is load-bearing: the data section is written in entry order, so a
// re-sorting container would still produce a valid archive, just not this one.
using Replacements = std::vector<std::pair<std::string, Bytes>>;

// Wrap raw bytes as a ShsCompression literal block: 0x1F + BE32 length + data.
// The game's decompressor copies them straight out, so a patched BMP costs
// 5 bytes of overhead and the archive keeps roughly its original size.
Bytes shs_wrap(const Bytes& data);

// DDP2 entry names are synthesised: "<archive stem>#00000", zero-padded to 5.
std::vector<ArchiveEntry> list_ddp2(const std::string& path);
std::vector<ArchiveEntry> list_ddp3(const std::string& path);
std::vector<ArchiveEntry> list_archive(const std::string& path);

// Rebuild `in_path` into `out_path`, substituting the named entries.  Entries
// that are not replaced keep their original compressed bytes verbatim; the
// section structure, entry sizes, names and padding are all preserved, so only
// the offset/size fields move.
void repack_ddp2(const std::string& in_path, const std::string& out_path,
                 const Replacements& replacements);
void repack_ddp3(const std::string& in_path, const std::string& out_path,
                 const Replacements& replacements);
// Dispatches on the 4-byte magic.
void repack(const std::string& in_path, const std::string& out_path,
            const Replacements& replacements);

}  // namespace shin::ddp
