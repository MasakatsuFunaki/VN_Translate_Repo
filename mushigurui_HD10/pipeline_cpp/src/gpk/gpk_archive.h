// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// BCmkri / System-NNN GPK+GTB archive reader and repacker.
// Pure byte plumbing, no image knowledge beyond magic-number sniffing.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "common/util.h"

namespace mgi::gpk {

constexpr std::size_t GPK_HEADER_SIZE = 64;

// 16 bytes: "PACKTYPE=8A     " -- the only packtype observed in shipped data.
extern const std::array<std::uint8_t, 16> PACKTYPE_DEFAULT;
// 8 bytes: "over2G!\0".  NOT a const char* -- the embedded NUL would truncate
// it to 7 and every has_64bit search would silently fail (R7).
extern const std::array<std::uint8_t, 8> TRAILER_MARKER;

struct GtbEntry {
    std::string name;      // UTF-8, lossily decoded from CP932 (U+FFFD per bad byte)
    std::uint32_t offset;  // low 32 bits of the GPK offset -- the table the reader uses
};

struct Gtb {
    std::uint32_t count = 0;
    std::vector<GtbEntry> entries;
    bool has_64bit = false;
};

Gtb read_gtb(const std::string& gtb_path);
void write_gtb(const std::string& gtb_path, const std::vector<GtbEntry>& entries,
               bool has_64bit = true);

struct EntryHeader {
    std::string type_tag;  // rstrip of trailing 0x20/0x00, then ASCII w/ U+FFFD
    std::array<std::uint8_t, 16> type_raw{};
    std::array<std::uint8_t, 16> meta{};
    std::uint32_t packed_size = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::array<std::uint8_t, 16> packtype{};
    std::string name;             // filled in by read_archive
    std::uint32_t offset = 0;     // filled in by read_archive
};

EntryHeader read_entry_header(const Bytes& gpk_buf, std::size_t offset);

struct Archive {
    Gtb gtb;
    std::vector<EntryHeader> entries;
    std::size_t gpk_size = 0;
    Bytes gpk_buf;
};

Archive read_archive(const std::string& gpk_path, const std::string& gtb_path);

// Payload bytes for the entry at `offset`, header stripped.  The header read
// throws when fewer than 64 bytes remain; the payload slice itself CLAMPS at
// end-of-buffer rather than throwing -- the last entry's packed_size is allowed
// to overrun the file in shipped archives.
Bytes extract_entry(const Bytes& gpk_buf, std::size_t offset);

std::array<std::uint8_t, 16> detect_type_tag(const Bytes& data);
std::pair<std::uint32_t, std::uint32_t> detect_dims(const Bytes& data);
Bytes build_entry(const EntryHeader& orig, const Bytes& payload);

// Rebuild a GPK+GTB pair, substituting the named entries.  Untouched entries
// are copied verbatim; an empty replacement set reproduces the input exactly.
void repack(const std::string& gpk_in, const std::string& gtb_in,
            const std::string& gpk_out, const std::string& gtb_out,
            const std::vector<std::pair<std::string, Bytes>>& replacements);

}  // namespace mgi::gpk
