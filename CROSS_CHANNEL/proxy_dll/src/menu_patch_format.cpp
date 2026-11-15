// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu_patch_format.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>

namespace menu_patch {

namespace {

using Bytes = std::vector<std::uint8_t>;

constexpr std::uint32_t kMurmurMultiplier = 0x5BD1E995u;
constexpr int kMurmurShiftR = 24;
constexpr int kMurmurFinalShift1 = 13;
constexpr int kMurmurFinalShift2 = 15;

constexpr std::uint32_t kXorStreamMultiplier = 0x9Eu;
constexpr std::uint32_t kXorStreamAddend = 0x37u;

constexpr std::size_t kWordBytes = 4u;
constexpr std::size_t kQwordBytes = 8u;
constexpr std::size_t kPakDirRecordSize = 16u;
constexpr std::size_t kPatchRecordHeaderSize = 28u;
constexpr std::size_t kLzssRingMask = kLzssRingSize - 1u;
constexpr std::size_t kLzssMaxLookback = kLzssRingSize - kLzssMaxMatch; // 4078 (0xFEE)
constexpr std::size_t kLzssHashSize = 4096u;
constexpr std::size_t kLzssHashMask = kLzssHashSize - 1u;
constexpr int kLzssMaxChainSteps = 96;
constexpr std::uint32_t kMaxSaneLzssOutputBytes = 256u * 1024u * 1024u;

constexpr std::size_t kCpkPacketHeaderSize = 16u;
constexpr std::size_t kUtfMinHeaderSize = 32u;
constexpr std::size_t kUtfBaseOffset = 8u;
constexpr std::size_t kUtfSchemaOffset = 32u;

constexpr std::uint8_t kUtfStorageZero = 0x10u;
constexpr std::uint8_t kUtfStorageConst = 0x30u;
constexpr std::uint8_t kUtfStoragePerRow = 0x50u;
constexpr std::uint8_t kUtfStorageMask = 0xF0u;
constexpr std::uint8_t kUtfTypeMask = 0x0Fu;

constexpr std::uint8_t kUtfTypeU8 = 0x0u;
constexpr std::uint8_t kUtfTypeS8 = 0x1u;
constexpr std::uint8_t kUtfTypeU16 = 0x2u;
constexpr std::uint8_t kUtfTypeS16 = 0x3u;
constexpr std::uint8_t kUtfTypeU32 = 0x4u;
constexpr std::uint8_t kUtfTypeS32 = 0x5u;
constexpr std::uint8_t kUtfTypeU64 = 0x6u;
constexpr std::uint8_t kUtfTypeS64 = 0x7u;
constexpr std::uint8_t kUtfTypeF32 = 0x8u;
constexpr std::uint8_t kUtfTypeString = 0xAu;
constexpr std::uint8_t kUtfTypeData = 0xBu;

std::uint16_t ReadBe16(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(p[0]) << 8) |
        static_cast<std::uint16_t>(p[1]));
}

std::uint32_t ReadBe32(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

std::uint64_t ReadBe64(const std::uint8_t* p) noexcept {
    return (static_cast<std::uint64_t>(ReadBe32(p)) << 32) |
           static_cast<std::uint64_t>(ReadBe32(p + 4));
}

void WriteBe16(std::uint8_t* p, std::uint16_t v) noexcept {
    p[0] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
    p[1] = static_cast<std::uint8_t>(v & 0xFFu);
}

void WriteBe32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
    p[1] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
    p[2] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
    p[3] = static_cast<std::uint8_t>(v & 0xFFu);
}

void WriteBe64(std::uint8_t* p, std::uint64_t v) noexcept {
    WriteBe32(p, static_cast<std::uint32_t>((v >> 32) & 0xFFFFFFFFu));
    WriteBe32(p + 4, static_cast<std::uint32_t>(v & 0xFFFFFFFFu));
}

std::uint32_t ReadLe32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t ReadLe64(const std::uint8_t* p) noexcept {
    return static_cast<std::uint64_t>(ReadLe32(p)) |
           (static_cast<std::uint64_t>(ReadLe32(p + 4)) << 32);
}

void AppendLe32(Bytes& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
}

void AppendLe64(Bytes& out, std::uint64_t v) {
    AppendLe32(out, static_cast<std::uint32_t>(v & 0xFFFFFFFFu));
    AppendLe32(out, static_cast<std::uint32_t>((v >> 32) & 0xFFFFFFFFu));
}

void WriteLe32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v & 0xFFu);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
    p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
    p[3] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
}

std::uint8_t ComputeMaskByte(const Bytes& orig_bytes,
                             std::uint32_t orig_hash,
                             std::size_t byte_index) noexcept {
    const auto idx32 = static_cast<std::uint32_t>(byte_index);
    const std::uint32_t shift = (idx32 & 3u) * 8u;
    const auto hash_byte = static_cast<std::uint8_t>((orig_hash >> shift) & 0xFFu);
    const auto stream_byte = static_cast<std::uint8_t>(
        (idx32 * kXorStreamMultiplier + kXorStreamAddend) & 0xFFu);
    const std::uint8_t orig_byte =
        orig_bytes.empty() ? 0u : orig_bytes[byte_index % orig_bytes.size()];
    return static_cast<std::uint8_t>(orig_byte ^ hash_byte ^ stream_byte);
}

std::size_t UtfValueByteWidth(std::uint8_t type) noexcept {
    switch (type) {
        case kUtfTypeU8:
        case kUtfTypeS8:
            return 1u;
        case kUtfTypeU16:
        case kUtfTypeS16:
            return 2u;
        case kUtfTypeU32:
        case kUtfTypeS32:
        case kUtfTypeF32:
        case kUtfTypeString:
            return 4u;
        case kUtfTypeU64:
        case kUtfTypeS64:
        case kUtfTypeData:
            return 8u;
        default:
            return 0u;
    }
}

struct UtfFieldLoc {
    std::uint8_t storage = 0;
    std::uint8_t type = 0;
    std::size_t file_offset = 0; // For CONST: offset in schema; for PER_ROW: row0 + col_off
    std::size_t row_rel_offset = 0;
};

struct ParsedUtfTable {
    std::string table_name;
    std::size_t file_base = 0;
    std::size_t rows_base = 0;
    std::size_t strs_base = 0;
    std::size_t strs_limit = 0;
    std::size_t data_base = 0;
    std::size_t table_end = 0;
    std::uint16_t row_len = 0;
    std::uint32_t num_rows = 0;
    std::map<std::string, UtfFieldLoc> cols;
};

bool ReadNullTermString(const Bytes& buf,
                        std::size_t strs_base,
                        std::size_t strs_limit,
                        std::uint32_t rel_off,
                        std::string& out) {
    if (rel_off >= strs_limit - strs_base) {
        return false;
    }
    const std::size_t start = strs_base + rel_off;
    std::size_t end = start;
    while (end < strs_limit && buf[end] != 0u) {
        ++end;
    }
    if (end >= strs_limit) {
        return false;
    }
    out.assign(reinterpret_cast<const char*>(buf.data() + start), end - start);
    return true;
}

bool ParseUtfTableAt(const Bytes& cpk,
                     std::size_t utf_file_off,
                     std::size_t utf_max_len,
                     ParsedUtfTable& out) {
    out = ParsedUtfTable{};
    if (utf_max_len < kUtfMinHeaderSize ||
        utf_file_off > cpk.size() ||
        utf_max_len > cpk.size() - utf_file_off) {
        return false;
    }
    const std::uint8_t* base = cpk.data() + utf_file_off;
    if (std::memcmp(base, "@UTF", 4) != 0) {
        return false;
    }
    const std::uint32_t table_size = ReadBe32(base + 4);
    if (static_cast<std::uint64_t>(table_size) + kUtfBaseOffset > utf_max_len) {
        return false;
    }

    const std::size_t body_base = utf_file_off + kUtfBaseOffset;
    const std::size_t table_end = body_base + table_size;
    const std::uint32_t rows_off = ReadBe32(base + 0x08);
    const std::uint32_t strs_off = ReadBe32(base + 0x0C);
    const std::uint32_t data_off = ReadBe32(base + 0x10);
    const std::uint32_t tname_off = ReadBe32(base + 0x14);
    const std::uint16_t num_cols = ReadBe16(base + 0x18);
    const std::uint16_t row_len = ReadBe16(base + 0x1A);
    const std::uint32_t num_rows = ReadBe32(base + 0x1C);

    if (rows_off > table_size || strs_off > table_size || data_off > table_size ||
        strs_off > data_off) {
        return false;
    }
    const std::uint64_t total_row_bytes =
        static_cast<std::uint64_t>(row_len) * static_cast<std::uint64_t>(num_rows);
    if (total_row_bytes > table_size - rows_off) {
        return false;
    }

    out.file_base = utf_file_off;
    out.rows_base = body_base + rows_off;
    out.strs_base = body_base + strs_off;
    out.strs_limit = body_base + data_off;
    out.data_base = body_base + data_off;
    out.table_end = table_end;
    out.row_len = row_len;
    out.num_rows = num_rows;

    if (!ReadNullTermString(cpk, out.strs_base, out.strs_limit, tname_off, out.table_name)) {
        return false;
    }

    std::size_t schema_cur = utf_file_off + kUtfSchemaOffset;
    const std::size_t schema_limit = out.rows_base;
    std::size_t row_rel = 0;

    for (std::uint16_t c = 0; c < num_cols; ++c) {
        if (schema_cur + 5u > schema_limit) {
            return false;
        }
        const std::uint8_t flags = cpk[schema_cur];
        const std::uint32_t cname_off = ReadBe32(cpk.data() + schema_cur + 1u);
        schema_cur += 5u;

        std::string cname;
        if (!ReadNullTermString(cpk, out.strs_base, out.strs_limit, cname_off, cname)) {
            return false;
        }

        const std::uint8_t storage = static_cast<std::uint8_t>(flags & kUtfStorageMask);
        const std::uint8_t type = static_cast<std::uint8_t>(flags & kUtfTypeMask);
        const std::size_t width = UtfValueByteWidth(type);
        if (width == 0u) {
            return false;
        }

        UtfFieldLoc loc{};
        loc.storage = storage;
        loc.type = type;

        if (storage == kUtfStorageZero) {
            out.cols[cname] = loc;
            continue;
        }
        if (storage == kUtfStorageConst) {
            if (schema_cur + width > schema_limit) {
                return false;
            }
            loc.file_offset = schema_cur;
            schema_cur += width;
            out.cols[cname] = loc;
            continue;
        }
        if (storage == kUtfStoragePerRow) {
            if (num_rows > 0u && row_rel + width > row_len) {
                return false;
            }
            loc.row_rel_offset = row_rel;
            loc.file_offset = out.rows_base + row_rel;
            row_rel += width;
            out.cols[cname] = loc;
            continue;
        }
        return false;
    }

    return row_rel == row_len || num_rows == 0u;
}

bool ReadUtfUintField(const Bytes& cpk,
                      const ParsedUtfTable& tbl,
                      std::uint32_t row_idx,
                      const char* col_name,
                      std::uint64_t& out_val,
                      std::size_t* out_mutable_file_off = nullptr) {
    const auto it = tbl.cols.find(col_name);
    if (it == tbl.cols.end() || row_idx >= tbl.num_rows) {
        return false;
    }
    const UtfFieldLoc& loc = it->second;
    if (loc.storage == kUtfStorageZero) {
        out_val = 0;
        return out_mutable_file_off == nullptr;
    }
    std::size_t foff = loc.file_offset;
    if (loc.storage == kUtfStoragePerRow) {
        foff = tbl.rows_base + static_cast<std::size_t>(row_idx) * tbl.row_len +
               loc.row_rel_offset;
    } else if (out_mutable_file_off != nullptr) {
        return false;
    }

    switch (loc.type) {
        case kUtfTypeU8:
        case kUtfTypeS8:
            out_val = cpk[foff];
            break;
        case kUtfTypeU16:
        case kUtfTypeS16:
            out_val = ReadBe16(cpk.data() + foff);
            break;
        case kUtfTypeU32:
        case kUtfTypeS32:
            out_val = ReadBe32(cpk.data() + foff);
            break;
        case kUtfTypeU64:
        case kUtfTypeS64:
            out_val = ReadBe64(cpk.data() + foff);
            break;
        default:
            return false;
    }
    if (out_mutable_file_off != nullptr) {
        *out_mutable_file_off = foff;
    }
    return true;
}

bool ReadUtfDataField(const Bytes& cpk,
                      const ParsedUtfTable& tbl,
                      std::uint32_t row_idx,
                      const char* col_name,
                      std::size_t& out_blob_off,
                      std::size_t& out_blob_len) {
    const auto it = tbl.cols.find(col_name);
    if (it == tbl.cols.end() || row_idx >= tbl.num_rows) {
        return false;
    }
    const UtfFieldLoc& loc = it->second;
    if (loc.type != kUtfTypeData) {
        return false;
    }
    if (loc.storage == kUtfStorageZero) {
        out_blob_off = tbl.data_base;
        out_blob_len = 0;
        return true;
    }
    std::size_t foff = loc.file_offset;
    if (loc.storage == kUtfStoragePerRow) {
        foff = tbl.rows_base + static_cast<std::size_t>(row_idx) * tbl.row_len +
               loc.row_rel_offset;
    }
    const std::uint32_t rel_off = ReadBe32(cpk.data() + foff);
    const std::uint32_t len = ReadBe32(cpk.data() + foff + 4u);
    if (static_cast<std::uint64_t>(rel_off) + len > tbl.table_end - tbl.data_base) {
        return false;
    }
    out_blob_off = tbl.data_base + rel_off;
    out_blob_len = len;
    return true;
}

struct CpkLayoutDetail {
    std::size_t content_offset = 0;
    std::uint32_t align = 0;
    std::size_t content_size_field_off = 0;
    std::size_t enabled_packed_field_off = 0;
    std::size_t enabled_data_field_off = 0;

    struct MutableTocEntry {
        CpkEntrySlice slice;
        std::size_t file_size_field_off = 0;
        std::size_t extract_size_field_off = 0;
    };
    std::vector<MutableTocEntry> entries;
};

bool InspectSysCpkLayout(const Bytes& cpk, CpkLayoutDetail& out) {
    out = CpkLayoutDetail{};
    if (cpk.size() < kCpkPacketHeaderSize + kUtfMinHeaderSize ||
        std::memcmp(cpk.data(), "CPK ", 4) != 0) {
        return false;
    }
    const std::uint64_t cpk_utf_len = ReadLe64(cpk.data() + 8u);
    if (cpk_utf_len > cpk.size() - kCpkPacketHeaderSize) {
        return false;
    }

    ParsedUtfTable hdr_tbl;
    if (!ParseUtfTableAt(cpk, kCpkPacketHeaderSize,
                         static_cast<std::size_t>(cpk_utf_len), hdr_tbl) ||
        hdr_tbl.num_rows != 1u) {
        return false;
    }

    std::uint64_t content_off = 0;
    std::uint64_t content_sz_dummy = 0;
    std::uint64_t itoc_off = 0;
    std::uint64_t itoc_sz = 0;
    std::uint64_t ep_dummy = 0;
    std::uint64_t ed_dummy = 0;
    std::uint64_t files_u64 = 0;
    std::uint64_t align_u64 = 0;

    if (!ReadUtfUintField(cpk, hdr_tbl, 0u, "ContentOffset", content_off) ||
        !ReadUtfUintField(cpk, hdr_tbl, 0u, "ContentSize", content_sz_dummy,
                          &out.content_size_field_off) ||
        !ReadUtfUintField(cpk, hdr_tbl, 0u, "ItocOffset", itoc_off) ||
        !ReadUtfUintField(cpk, hdr_tbl, 0u, "ItocSize", itoc_sz) ||
        !ReadUtfUintField(cpk, hdr_tbl, 0u, "EnabledPackedSize", ep_dummy,
                          &out.enabled_packed_field_off) ||
        !ReadUtfUintField(cpk, hdr_tbl, 0u, "EnabledDataSize", ed_dummy,
                          &out.enabled_data_field_off) ||
        !ReadUtfUintField(cpk, hdr_tbl, 0u, "Files", files_u64) ||
        !ReadUtfUintField(cpk, hdr_tbl, 0u, "Align", align_u64)) {
        return false;
    }

    if (align_u64 == 0u || (align_u64 & (align_u64 - 1u)) != 0u ||
        align_u64 > 0x100000u || content_off > cpk.size() ||
        itoc_off > cpk.size() || itoc_sz < kCpkPacketHeaderSize ||
        itoc_sz > cpk.size() - itoc_off || itoc_off + itoc_sz > content_off ||
        files_u64 == 0u || files_u64 > 65535u) {
        return false;
    }

    out.content_offset = static_cast<std::size_t>(content_off);
    out.align = static_cast<std::uint32_t>(align_u64);

    const auto itoc_pos = static_cast<std::size_t>(itoc_off);
    if (std::memcmp(cpk.data() + itoc_pos, "ITOC", 4) != 0) {
        return false;
    }
    const std::uint64_t itoc_utf_len = ReadLe64(cpk.data() + itoc_pos + 8u);
    if (itoc_utf_len > itoc_sz - kCpkPacketHeaderSize) {
        return false;
    }

    ParsedUtfTable itoc_tbl;
    if (!ParseUtfTableAt(cpk, itoc_pos + kCpkPacketHeaderSize,
                         static_cast<std::size_t>(itoc_utf_len), itoc_tbl) ||
        itoc_tbl.num_rows != 1u) {
        return false;
    }

    std::uint64_t files_l = 0;
    std::uint64_t files_h = 0;
    std::size_t data_l_off = 0;
    std::size_t data_l_len = 0;
    std::size_t data_h_off = 0;
    std::size_t data_h_len = 0;
    if (!ReadUtfUintField(cpk, itoc_tbl, 0u, "FilesL", files_l) ||
        !ReadUtfUintField(cpk, itoc_tbl, 0u, "FilesH", files_h) ||
        !ReadUtfDataField(cpk, itoc_tbl, 0u, "DataL", data_l_off, data_l_len) ||
        !ReadUtfDataField(cpk, itoc_tbl, 0u, "DataH", data_h_off, data_h_len)) {
        return false;
    }

    if (files_l + files_h != files_u64) {
        return false;
    }

    std::vector<CpkLayoutDetail::MutableTocEntry> by_id(
        static_cast<std::size_t>(files_u64));
    std::vector<bool> seen(static_cast<std::size_t>(files_u64), false);

    auto collect_table = [&](std::size_t blob_off,
                             std::size_t blob_len,
                             std::uint64_t expected_rows,
                             bool is_low) -> bool {
        if (expected_rows == 0u && blob_len == 0u) {
            return true;
        }
        ParsedUtfTable sub_tbl;
        if (!ParseUtfTableAt(cpk, blob_off, blob_len, sub_tbl) ||
            sub_tbl.num_rows != expected_rows) {
            return false;
        }
        for (std::uint32_t r = 0; r < sub_tbl.num_rows; ++r) {
            std::uint64_t id_u64 = 0;
            std::uint64_t fsz_u64 = 0;
            std::uint64_t esz_u64 = 0;
            std::size_t fsz_off = 0;
            std::size_t esz_off = 0;
            if (!ReadUtfUintField(cpk, sub_tbl, r, "ID", id_u64) ||
                !ReadUtfUintField(cpk, sub_tbl, r, "FileSize", fsz_u64, &fsz_off) ||
                !ReadUtfUintField(cpk, sub_tbl, r, "ExtractSize", esz_u64, &esz_off)) {
                return false;
            }
            if (id_u64 >= files_u64 || seen[static_cast<std::size_t>(id_u64)] ||
                fsz_u64 != esz_u64 || fsz_u64 > 0xFFFFFFFFull) {
                return false;
            }
            seen[static_cast<std::size_t>(id_u64)] = true;
            auto& slot = by_id[static_cast<std::size_t>(id_u64)];
            slot.slice.id = static_cast<std::uint16_t>(id_u64);
            slot.slice.is_low_table = is_low;
            slot.slice.size = static_cast<std::uint32_t>(fsz_u64);
            slot.file_size_field_off = fsz_off;
            slot.extract_size_field_off = esz_off;
        }
        return true;
    };

    if (!collect_table(data_l_off, data_l_len, files_l, true) ||
        !collect_table(data_h_off, data_h_len, files_h, false)) {
        return false;
    }

    std::uint64_t cur_off = content_off;
    const std::uint64_t align64 = out.align;
    for (std::size_t i = 0; i < by_id.size(); ++i) {
        if (!seen[i]) {
            return false;
        }
        by_id[i].slice.offset = cur_off;
        const std::uint64_t end_off = cur_off + by_id[i].slice.size;
        if (end_off > cpk.size()) {
            return false;
        }
        if (i + 1 < by_id.size()) {
            cur_off = (end_off + (align64 - 1u)) & ~(align64 - 1u);
            if (cur_off > cpk.size()) {
                return false;
            }
        } else {
            cur_off = end_off;
        }
    }

    out.entries = std::move(by_id);
    return true;
}

struct StoredPatchRecord {
    std::uint32_t cpk_entry_id = 0;
    std::uint32_t pak_sub_index = 0;
    std::uint32_t orig_size = 0;
    std::uint32_t orig_hash = 0;
    std::uint32_t new_size = 0;
    std::uint32_t new_hash = 0;
    Bytes masked_payload;
};

}  // namespace

// --- MurmurHash2 ------------------------------------------------------------

std::uint32_t ComputeMurmurHash2(const std::uint8_t* data,
                                 std::size_t size) noexcept {
    if (data == nullptr && size != 0u) {
        return 0u;
    }
    auto h = static_cast<std::uint32_t>(size);
    std::size_t i = 0;
    while (i + kWordBytes <= size) {
        std::uint32_t k = ReadLe32(data + i);
        k *= kMurmurMultiplier;
        k ^= k >> kMurmurShiftR;
        k *= kMurmurMultiplier;
        h *= kMurmurMultiplier;
        h ^= k;
        i += kWordBytes;
    }
    const std::size_t rem = size - i;
    if (rem == 3u) {
        h ^= static_cast<std::uint32_t>(data[i + 2u]) << 16;
    }
    if (rem >= 2u) {
        h ^= static_cast<std::uint32_t>(data[i + 1u]) << 8;
    }
    if (rem >= 1u) {
        h ^= static_cast<std::uint32_t>(data[i]);
        h *= kMurmurMultiplier;
    }
    h ^= h >> kMurmurFinalShift1;
    h *= kMurmurMultiplier;
    h ^= h >> kMurmurFinalShift2;
    return h;
}

std::uint32_t ComputeMurmurHash2(const Bytes& data) noexcept {
    return ComputeMurmurHash2(data.data(), data.size());
}

// --- Zero-Ring LZSS ---------------------------------------------------------

bool LzssDecompressZeroRing(const std::uint8_t* src,
                            std::size_t src_size,
                            Bytes& out) {
    out.clear();
    if (src == nullptr || src_size < kWordBytes) {
        return false;
    }
    const std::uint32_t uncomp_size = ReadLe32(src);
    if (uncomp_size > kMaxSaneLzssOutputBytes) {
        return false;
    }
    if (uncomp_size == 0u) {
        return true;
    }

    out.reserve(uncomp_size);
    std::array<std::uint8_t, kLzssRingSize> ring{};
    std::size_t ring_pos = kLzssInitialRingPos;
    std::uint32_t flags = 0u;
    std::size_t pos = kWordBytes;

    while (out.size() < uncomp_size) {
        flags >>= 1;
        if ((flags & 0x100u) == 0u) {
            if (pos >= src_size) {
                return false;
            }
            flags = static_cast<std::uint32_t>(src[pos++]) | 0xFF00u;
        }

        if ((flags & 1u) != 0u) {
            if (pos >= src_size) {
                return false;
            }
            const std::uint8_t b = src[pos++];
            out.push_back(b);
            ring[ring_pos] = b;
            ring_pos = (ring_pos + 1u) & kLzssRingMask;
            continue;
        }

        if (pos + 1u >= src_size) {
            return false;
        }
        const std::uint32_t b0 = src[pos++];
        const std::uint32_t b1 = src[pos++];
        const std::size_t match_pos =
            static_cast<std::size_t>(b0 | ((b1 & 0xF0u) << 4));
        const std::size_t match_len =
            static_cast<std::size_t>((b1 & 0x0Fu) + kLzssMinMatch);

        for (std::size_t k = 0; k < match_len && out.size() < uncomp_size; ++k) {
            const std::uint8_t b = ring[(match_pos + k) & kLzssRingMask];
            out.push_back(b);
            ring[ring_pos] = b;
            ring_pos = (ring_pos + 1u) & kLzssRingMask;
        }
    }

    return true;
}

bool LzssDecompressZeroRing(const Bytes& src, Bytes& out) {
    return LzssDecompressZeroRing(src.data(), src.size(), out);
}

bool LzssCompressZeroRing(const std::uint8_t* src,
                          std::size_t src_size,
                          Bytes& out) {
    out.clear();
    if ((src == nullptr && src_size != 0u) ||
        src_size > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
        return false;
    }

    AppendLe32(out, static_cast<std::uint32_t>(src_size));
    if (src_size == 0u) {
        return true;
    }

    // Prepend 0xFEE zero bytes representing the initial 0x00-filled ring.
    Bytes work(kLzssInitialRingPos + src_size, 0u);
    std::memcpy(work.data() + kLzssInitialRingPos, src, src_size);
    const std::size_t total = work.size();

    auto hash3 = [&](std::size_t p) noexcept -> std::size_t {
        return ((static_cast<std::size_t>(work[p]) << 5) ^
                (static_cast<std::size_t>(work[p + 1u]) << 2) ^
                static_cast<std::size_t>(work[p + 2u])) &
               kLzssHashMask;
    };

    std::array<std::int32_t, kLzssHashSize> head;
    head.fill(-1);
    std::array<std::int32_t, kLzssRingSize> prev;
    prev.fill(-1);

    auto insert_pos = [&](std::size_t p) noexcept {
        if (p + 2u >= total) {
            return;
        }
        const std::size_t h = hash3(p);
        prev[p & kLzssRingMask] = head[h];
        head[h] = static_cast<std::int32_t>(p);
    };

    // Seed the three distinct 3-byte suffixes of the initial zero-filled ring:
    //   - kLzssInitialRingPos - kLzssMaxMatch (4060): followed by 18 zero bytes
    //   - kLzssInitialRingPos - 2 (4076): {0x00, 0x00, src[0]}
    //   - kLzssInitialRingPos - 1 (4077): {0x00, src[0], src[1]}
    insert_pos(kLzssInitialRingPos - kLzssMaxMatch);
    if (src_size >= 1u) {
        insert_pos(kLzssInitialRingPos - 2u);
    }
    if (src_size >= 2u) {
        insert_pos(kLzssInitialRingPos - 1u);
    }

    out.reserve(kWordBytes + src_size + (src_size / 8u) + 16u);

    std::size_t flag_byte_index = out.size();
    out.push_back(0u);
    std::uint8_t flag_mask = 1u;

    std::size_t cur = kLzssInitialRingPos;
    while (cur < total) {
        if (flag_mask == 0u) {
            flag_byte_index = out.size();
            out.push_back(0u);
            flag_mask = 1u;
        }

        const std::size_t max_len = std::min(kLzssMaxMatch, total - cur);
        std::size_t best_len = 0u;
        std::size_t best_src = 0u;

        if (max_len >= kLzssMinMatch) {
            std::int32_t cand = head[hash3(cur)];
            int steps = 0;
            while (cand >= 0 && steps < kLzssMaxChainSteps) {
                const auto cand_pos = static_cast<std::size_t>(cand);
                const std::size_t dist = cur - cand_pos;
                if (dist == 0u || dist > kLzssMaxLookback) {
                    break;
                }
                if (work[cand_pos + best_len] == work[cur + best_len]) {
                    std::size_t len = 0u;
                    while (len < max_len &&
                           work[cand_pos + len] == work[cur + len]) {
                        ++len;
                    }
                    if (len >= kLzssMinMatch && len > best_len) {
                        best_len = len;
                        best_src = cand_pos;
                        if (best_len == max_len) {
                            break;
                        }
                    }
                }
                cand = prev[cand_pos & kLzssRingMask];
                ++steps;
            }
        }

        if (best_len >= kLzssMinMatch) {
            const auto ring_off = static_cast<std::uint32_t>(best_src & kLzssRingMask);
            const auto encoded_len = static_cast<std::uint32_t>(best_len - kLzssMinMatch);
            out.push_back(static_cast<std::uint8_t>(ring_off & 0xFFu));
            out.push_back(static_cast<std::uint8_t>(
                ((ring_off >> 4) & 0xF0u) | (encoded_len & 0x0Fu)));
            for (std::size_t k = 0; k < best_len; ++k) {
                insert_pos(cur + k);
            }
            cur += best_len;
        } else {
            out[flag_byte_index] = static_cast<std::uint8_t>(
                out[flag_byte_index] | flag_mask);
            out.push_back(work[cur]);
            insert_pos(cur);
            ++cur;
        }

        flag_mask = static_cast<std::uint8_t>(flag_mask << 1);
    }

    return true;
}

bool LzssCompressZeroRing(const Bytes& src, Bytes& out) {
    return LzssCompressZeroRing(src.data(), src.size(), out);
}

// --- Inner .pak Container ---------------------------------------------------

bool ParsePakArchive(const Bytes& pak_bytes,
                     std::vector<PakEntrySlice>& out_entries) {
    out_entries.clear();
    if (pak_bytes.size() < kPakDirRecordSize) {
        return false;
    }

    const std::uint32_t first_off = ReadLe32(pak_bytes.data());
    if (first_off < kPakDirRecordSize ||
        (first_off % kPakDirRecordSize) != 0u ||
        first_off > pak_bytes.size()) {
        return false;
    }

    const std::uint32_t count = first_off / static_cast<std::uint32_t>(kPakDirRecordSize);
    out_entries.reserve(count);

    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t* rec = pak_bytes.data() + i * kPakDirRecordSize;
        const std::uint32_t off = ReadLe32(rec);
        const std::uint32_t sz = ReadLe32(rec + 4u);
        const std::uint32_t rsv0 = ReadLe32(rec + 8u);
        const std::uint32_t rsv1 = ReadLe32(rec + 12u);
        if (rsv0 != 0u || rsv1 != 0u || off < first_off ||
            static_cast<std::uint64_t>(off) + sz > pak_bytes.size()) {
            out_entries.clear();
            return false;
        }
        out_entries.push_back(PakEntrySlice{off, sz});
    }

    return true;
}

bool RebuildPakArchive(
    const Bytes& orig_pak,
    const std::vector<std::pair<std::uint32_t, Bytes>>& replacements,
    Bytes& out_pak) {
    out_pak.clear();
    std::vector<PakEntrySlice> slices;
    if (!ParsePakArchive(orig_pak, slices)) {
        return false;
    }

    std::map<std::uint32_t, const Bytes*> rep_map;
    for (const auto& kv : replacements) {
        if (kv.first >= slices.size() ||
            !rep_map.emplace(kv.first, &kv.second).second) {
            return false;
        }
    }

    const std::size_t dir_bytes = slices.size() * kPakDirRecordSize;
    std::uint64_t total_size = dir_bytes;
    for (std::size_t i = 0; i < slices.size(); ++i) {
        const auto it = rep_map.find(static_cast<std::uint32_t>(i));
        total_size += (it != rep_map.end()) ? it->second->size() : slices[i].size;
    }
    if (total_size > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())) {
        return false;
    }

    out_pak.assign(static_cast<std::size_t>(total_size), 0u);
    auto cur_off = static_cast<std::uint32_t>(dir_bytes);
    for (std::size_t i = 0; i < slices.size(); ++i) {
        const auto it = rep_map.find(static_cast<std::uint32_t>(i));
        const std::uint8_t* payload_ptr =
            (it != rep_map.end())
                ? it->second->data()
                : (orig_pak.data() + slices[i].offset);
        const std::size_t payload_sz =
            (it != rep_map.end()) ? it->second->size() : slices[i].size;

        std::uint8_t* rec = out_pak.data() + i * kPakDirRecordSize;
        WriteLe32(rec, cur_off);
        WriteLe32(rec + 4u, static_cast<std::uint32_t>(payload_sz));
        if (payload_sz > 0u) {
            std::memcpy(out_pak.data() + cur_off, payload_ptr, payload_sz);
        }
        cur_off += static_cast<std::uint32_t>(payload_sz);
    }

    return true;
}

// --- Criware CPK TOC & Rebuilder --------------------------------------------

bool ParseSysCpkToc(const Bytes& cpk_bytes,
                    std::vector<CpkEntrySlice>& out_entries) {
    out_entries.clear();
    CpkLayoutDetail layout;
    if (!InspectSysCpkLayout(cpk_bytes, layout)) {
        return false;
    }
    out_entries.reserve(layout.entries.size());
    for (const auto& item : layout.entries) {
        out_entries.push_back(item.slice);
    }
    return true;
}

bool RebuildSysCpk(
    const Bytes& orig_cpk,
    const std::vector<std::pair<std::uint32_t, Bytes>>& entry_replacements,
    Bytes& out_patched_cpk) {
    out_patched_cpk.clear();
    CpkLayoutDetail layout;
    if (!InspectSysCpkLayout(orig_cpk, layout)) {
        return false;
    }

    std::map<std::uint32_t, const Bytes*> rep_map;
    for (const auto& kv : entry_replacements) {
        if (kv.first >= layout.entries.size() ||
            !rep_map.emplace(kv.first, &kv.second).second) {
            return false;
        }
    }

    const std::uint64_t align64 = layout.align;
    std::uint64_t sum_sizes = 0;
    std::uint64_t content_span = 0;

    for (std::size_t i = 0; i < layout.entries.size(); ++i) {
        const auto it = rep_map.find(static_cast<std::uint32_t>(i));
        const std::uint64_t sz =
            (it != rep_map.end()) ? it->second->size() : layout.entries[i].slice.size;
        if (layout.entries[i].slice.is_low_table && sz > 0xFFFFu) {
            return false;
        }
        if (sz > 0xFFFFFFFFull) {
            return false;
        }
        sum_sizes += sz;
        if (i + 1 < layout.entries.size()) {
            content_span += (sz + (align64 - 1u)) & ~(align64 - 1u);
        } else {
            content_span += sz;
        }
    }

    const std::uint64_t total_file_size = layout.content_offset + content_span;
    if (total_file_size >
        static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return false;
    }

    const std::uint64_t content_size_aligned =
        (content_span + (align64 - 1u)) & ~(align64 - 1u);

    // Copy [0 .. ContentOffset) verbatim so CpkHeader, (c)CRI, and ITOC stay
    // byte-identical apart from the updated size columns.
    out_patched_cpk.assign(orig_cpk.begin(),
                           orig_cpk.begin() +
                               static_cast<std::ptrdiff_t>(layout.content_offset));
    out_patched_cpk.reserve(static_cast<std::size_t>(total_file_size));

    WriteBe64(out_patched_cpk.data() + layout.content_size_field_off,
              content_size_aligned);
    WriteBe64(out_patched_cpk.data() + layout.enabled_packed_field_off,
              sum_sizes);
    WriteBe64(out_patched_cpk.data() + layout.enabled_data_field_off,
              sum_sizes);

    for (std::size_t i = 0; i < layout.entries.size(); ++i) {
        const auto& meta = layout.entries[i];
        const auto it = rep_map.find(static_cast<std::uint32_t>(i));
        const std::uint8_t* src_ptr =
            (it != rep_map.end())
                ? it->second->data()
                : (orig_cpk.data() + static_cast<std::size_t>(meta.slice.offset));
        const auto sz = static_cast<std::uint32_t>(
            (it != rep_map.end()) ? it->second->size() : meta.slice.size);

        if (meta.slice.is_low_table) {
            WriteBe16(out_patched_cpk.data() + meta.file_size_field_off,
                      static_cast<std::uint16_t>(sz));
            WriteBe16(out_patched_cpk.data() + meta.extract_size_field_off,
                      static_cast<std::uint16_t>(sz));
        } else {
            WriteBe32(out_patched_cpk.data() + meta.file_size_field_off, sz);
            WriteBe32(out_patched_cpk.data() + meta.extract_size_field_off, sz);
        }

        out_patched_cpk.insert(out_patched_cpk.end(), src_ptr, src_ptr + sz);
        if (i + 1 < layout.entries.size()) {
            const std::size_t rem = sz % layout.align;
            if (rem != 0u) {
                out_patched_cpk.insert(out_patched_cpk.end(),
                                       layout.align - rem, 0u);
            }
        }
    }

    return true;
}

// --- MenuPatchPackage (PIMPL) -----------------------------------------------

struct MenuPatchPackage::Impl {
    bool loaded = false;
    PatchHeaderInfo header{};
    std::vector<StoredPatchRecord> records;
};

MenuPatchPackage::MenuPatchPackage() : impl_(std::make_unique<Impl>()) {}
MenuPatchPackage::~MenuPatchPackage() = default;
MenuPatchPackage::MenuPatchPackage(MenuPatchPackage&&) noexcept = default;
MenuPatchPackage& MenuPatchPackage::operator=(MenuPatchPackage&&) noexcept = default;

bool MenuPatchPackage::ParseHeaderPrefix(const std::uint8_t* data,
                                         std::size_t size,
                                         PatchHeaderInfo& out_info) noexcept {
    out_info = PatchHeaderInfo{};
    if (data == nullptr || size < kPatchHeaderSize ||
        std::memcmp(data, kPatchMagic, kPatchMagicSize) != 0) {
        return false;
    }
    const std::uint32_t ver = ReadLe32(data + 4u);
    if (ver != kPatchFormatVersion) {
        return false;
    }
    out_info.version = ver;
    out_info.record_count = ReadLe32(data + 8u);
    out_info.patch_payload_hash = ReadLe32(data + 12u);
    out_info.expected_orig_cpk_size = ReadLe64(data + 16u);
    out_info.expected_patched_cpk_size = ReadLe64(data + 24u);
    return true;
}

bool MenuPatchPackage::LoadFromMemory(const std::uint8_t* data,
                                      std::size_t size) {
    impl_->loaded = false;
    impl_->header = PatchHeaderInfo{};
    impl_->records.clear();

    PatchHeaderInfo hdr{};
    if (!ParseHeaderPrefix(data, size, hdr)) {
        return false;
    }

    const std::size_t payload_bytes = size - kPatchHeaderSize;
    const std::uint32_t actual_payload_hash =
        ComputeMurmurHash2(data + kPatchHeaderSize, payload_bytes);
    if (actual_payload_hash != hdr.patch_payload_hash) {
        return false;
    }

    std::vector<StoredPatchRecord> parsed;
    parsed.reserve(hdr.record_count);
    std::set<std::pair<std::uint32_t, std::uint32_t>> seen_targets;

    std::size_t off = kPatchHeaderSize;
    for (std::uint32_t i = 0; i < hdr.record_count; ++i) {
        if (size - off < kPatchRecordHeaderSize) {
            return false;
        }
        StoredPatchRecord rec;
        rec.cpk_entry_id = ReadLe32(data + off);
        rec.pak_sub_index = ReadLe32(data + off + 4u);
        rec.orig_size = ReadLe32(data + off + 8u);
        rec.orig_hash = ReadLe32(data + off + 12u);
        rec.new_size = ReadLe32(data + off + 16u);
        rec.new_hash = ReadLe32(data + off + 20u);
        const std::uint32_t masked_sz = ReadLe32(data + off + 24u);
        off += kPatchRecordHeaderSize;

        if (rec.new_size > kMaxSaneLzssOutputBytes ||
            size - off < masked_sz ||
            !seen_targets.emplace(rec.cpk_entry_id, rec.pak_sub_index).second) {
            return false;
        }
        rec.masked_payload.assign(data + off, data + off + masked_sz);
        off += masked_sz;
        parsed.push_back(std::move(rec));
    }

    if (off != size) {
        return false;
    }

    impl_->header = hdr;
    impl_->records = std::move(parsed);
    impl_->loaded = true;
    return true;
}

bool MenuPatchPackage::LoadFromMemory(const Bytes& patch_blob) {
    return LoadFromMemory(patch_blob.data(), patch_blob.size());
}

bool MenuPatchPackage::IsLoaded() const noexcept {
    return impl_ && impl_->loaded;
}

const PatchHeaderInfo& MenuPatchPackage::Header() const noexcept {
    static const PatchHeaderInfo kEmpty{};
    return impl_ ? impl_->header : kEmpty;
}

std::size_t MenuPatchPackage::RecordCount() const noexcept {
    return (impl_ && impl_->loaded) ? impl_->records.size() : 0u;
}

bool MenuPatchPackage::ApplyToSysCpk(const Bytes& orig_cpk,
                                     Bytes& out_patched_cpk) const {
    out_patched_cpk.clear();
    if (!IsLoaded() || orig_cpk.size() != impl_->header.expected_orig_cpk_size) {
        return false;
    }

    std::vector<CpkEntrySlice> cpk_toc;
    if (!ParseSysCpkToc(orig_cpk, cpk_toc)) {
        return false;
    }

    std::map<std::uint32_t, std::vector<const StoredPatchRecord*>> by_cpk_id;
    for (const auto& rec : impl_->records) {
        if (rec.cpk_entry_id >= cpk_toc.size()) {
            return false;
        }
        by_cpk_id[rec.cpk_entry_id].push_back(&rec);
    }

    std::vector<std::pair<std::uint32_t, Bytes>> cpk_replacements;
    cpk_replacements.reserve(by_cpk_id.size());

    for (const auto& group : by_cpk_id) {
        const std::uint32_t cpk_id = group.first;
        const CpkEntrySlice& outer = cpk_toc[cpk_id];

        Bytes orig_pak;
        if (!LzssDecompressZeroRing(
                orig_cpk.data() + static_cast<std::size_t>(outer.offset),
                outer.size,
                orig_pak)) {
            return false;
        }

        std::vector<PakEntrySlice> pak_slices;
        if (!ParsePakArchive(orig_pak, pak_slices)) {
            return false;
        }

        std::vector<std::pair<std::uint32_t, Bytes>> pak_replacements;
        pak_replacements.reserve(group.second.size());

        for (const StoredPatchRecord* rec : group.second) {
            if (rec->pak_sub_index >= pak_slices.size()) {
                return false;
            }
            const PakEntrySlice& sub = pak_slices[rec->pak_sub_index];
            if (sub.size != rec->orig_size) {
                return false;
            }
            const Bytes orig_sub(orig_pak.begin() + sub.offset,
                                 orig_pak.begin() + sub.offset + sub.size);
            if (ComputeMurmurHash2(orig_sub) != rec->orig_hash) {
                return false;
            }

            Bytes lzss_delta(rec->masked_payload.size());
            for (std::size_t j = 0; j < lzss_delta.size(); ++j) {
                lzss_delta[j] = static_cast<std::uint8_t>(
                    rec->masked_payload[j] ^
                    ComputeMaskByte(orig_sub, rec->orig_hash, j));
            }

            Bytes xor_delta;
            if (!LzssDecompressZeroRing(lzss_delta, xor_delta) ||
                xor_delta.size() != rec->new_size) {
                return false;
            }

            Bytes new_sub(rec->new_size);
            for (std::size_t i = 0; i < new_sub.size(); ++i) {
                const std::uint8_t base_byte =
                    orig_sub.empty() ? 0u : orig_sub[i % orig_sub.size()];
                new_sub[i] = static_cast<std::uint8_t>(xor_delta[i] ^ base_byte);
            }

            if (ComputeMurmurHash2(new_sub) != rec->new_hash) {
                return false;
            }

            pak_replacements.emplace_back(rec->pak_sub_index, std::move(new_sub));
        }

        Bytes rebuilt_pak;
        if (!RebuildPakArchive(orig_pak, pak_replacements, rebuilt_pak)) {
            return false;
        }

        Bytes recompressed_pak;
        if (!LzssCompressZeroRing(rebuilt_pak, recompressed_pak)) {
            return false;
        }

        cpk_replacements.emplace_back(cpk_id, std::move(recompressed_pak));
    }

    if (!RebuildSysCpk(orig_cpk, cpk_replacements, out_patched_cpk)) {
        return false;
    }

    if (out_patched_cpk.size() != impl_->header.expected_patched_cpk_size) {
        out_patched_cpk.clear();
        return false;
    }

    return true;
}

bool MenuPatchPackage::BuildPatchBlob(
    const Bytes& orig_cpk,
    const std::vector<SubTextureReplacement>& replacements,
    Bytes& out_patch_blob,
    Bytes* out_patched_cpk) {
    out_patch_blob.clear();
    if (out_patched_cpk != nullptr) {
        out_patched_cpk->clear();
    }

    std::vector<CpkEntrySlice> cpk_toc;
    if (!ParseSysCpkToc(orig_cpk, cpk_toc)) {
        return false;
    }

    // Sort deterministically by (cpk_entry_id, pak_sub_index) and reject duplicates.
    std::vector<const SubTextureReplacement*> sorted;
    sorted.reserve(replacements.size());
    for (const auto& r : replacements) {
        sorted.push_back(&r);
    }
    std::sort(sorted.begin(), sorted.end(),
              [](const SubTextureReplacement* a, const SubTextureReplacement* b) {
                  if (a->cpk_entry_id != b->cpk_entry_id) {
                      return a->cpk_entry_id < b->cpk_entry_id;
                  }
                  return a->pak_sub_index < b->pak_sub_index;
              });
    for (std::size_t i = 1; i < sorted.size(); ++i) {
        if (sorted[i - 1]->cpk_entry_id == sorted[i]->cpk_entry_id &&
            sorted[i - 1]->pak_sub_index == sorted[i]->pak_sub_index) {
            return false;
        }
    }

    // Verify every orig_bytes matches the actual sub-entry inside orig_cpk.
    std::map<std::uint32_t, std::vector<const SubTextureReplacement*>> by_cpk;
    for (const SubTextureReplacement* r : sorted) {
        if (r->cpk_entry_id >= cpk_toc.size()) {
            return false;
        }
        by_cpk[r->cpk_entry_id].push_back(r);
    }

    for (const auto& grp : by_cpk) {
        const CpkEntrySlice& outer = cpk_toc[grp.first];
        Bytes pak;
        if (!LzssDecompressZeroRing(
                orig_cpk.data() + static_cast<std::size_t>(outer.offset),
                outer.size,
                pak)) {
            return false;
        }
        std::vector<PakEntrySlice> pak_slices;
        if (!ParsePakArchive(pak, pak_slices)) {
            return false;
        }
        for (const SubTextureReplacement* r : grp.second) {
            if (r->pak_sub_index >= pak_slices.size()) {
                return false;
            }
            const PakEntrySlice& sub = pak_slices[r->pak_sub_index];
            if (sub.size != r->orig_bytes.size() ||
                std::memcmp(pak.data() + sub.offset,
                            r->orig_bytes.data(),
                            sub.size) != 0) {
                return false;
            }
        }
    }

    Bytes payload;
    for (const SubTextureReplacement* r : sorted) {
        const std::uint32_t orig_hash = ComputeMurmurHash2(r->orig_bytes);
        const std::uint32_t new_hash = ComputeMurmurHash2(r->new_bytes);

        Bytes xor_delta(r->new_bytes.size());
        for (std::size_t i = 0; i < xor_delta.size(); ++i) {
            const std::uint8_t base_byte =
                r->orig_bytes.empty()
                    ? 0u
                    : r->orig_bytes[i % r->orig_bytes.size()];
            xor_delta[i] = static_cast<std::uint8_t>(r->new_bytes[i] ^ base_byte);
        }

        Bytes lzss_delta;
        if (!LzssCompressZeroRing(xor_delta, lzss_delta)) {
            return false;
        }

        Bytes masked(lzss_delta.size());
        for (std::size_t j = 0; j < masked.size(); ++j) {
            masked[j] = static_cast<std::uint8_t>(
                lzss_delta[j] ^ ComputeMaskByte(r->orig_bytes, orig_hash, j));
        }

        AppendLe32(payload, r->cpk_entry_id);
        AppendLe32(payload, r->pak_sub_index);
        AppendLe32(payload, static_cast<std::uint32_t>(r->orig_bytes.size()));
        AppendLe32(payload, orig_hash);
        AppendLe32(payload, static_cast<std::uint32_t>(r->new_bytes.size()));
        AppendLe32(payload, new_hash);
        AppendLe32(payload, static_cast<std::uint32_t>(masked.size()));
        payload.insert(payload.end(), masked.begin(), masked.end());
    }

    // Build expected patched CPK via the exact same RebuildPak + LzssCompress +
    // RebuildSysCpk pipeline so expected_patched_cpk_size is exact, then prove
    // round-trip through LoadFromMemory + ApplyToSysCpk.
    std::vector<std::pair<std::uint32_t, Bytes>> cpk_reps;
    for (const auto& grp : by_cpk) {
        const CpkEntrySlice& outer = cpk_toc[grp.first];
        Bytes pak;
        if (!LzssDecompressZeroRing(
                orig_cpk.data() + static_cast<std::size_t>(outer.offset),
                outer.size,
                pak)) {
            return false;
        }
        std::vector<std::pair<std::uint32_t, Bytes>> pak_reps;
        for (const SubTextureReplacement* r : grp.second) {
            pak_reps.emplace_back(r->pak_sub_index, r->new_bytes);
        }
        Bytes rebuilt_pak;
        Bytes comp_pak;
        if (!RebuildPakArchive(pak, pak_reps, rebuilt_pak) ||
            !LzssCompressZeroRing(rebuilt_pak, comp_pak)) {
            return false;
        }
        cpk_reps.emplace_back(grp.first, std::move(comp_pak));
    }

    Bytes rebuilt_cpk;
    if (!RebuildSysCpk(orig_cpk, cpk_reps, rebuilt_cpk)) {
        return false;
    }

    const std::uint32_t payload_hash = ComputeMurmurHash2(payload);
    out_patch_blob.reserve(kPatchHeaderSize + payload.size());
    out_patch_blob.insert(out_patch_blob.end(),
                          kPatchMagic,
                          kPatchMagic + kPatchMagicSize);
    AppendLe32(out_patch_blob, kPatchFormatVersion);
    AppendLe32(out_patch_blob, static_cast<std::uint32_t>(sorted.size()));
    AppendLe32(out_patch_blob, payload_hash);
    AppendLe64(out_patch_blob, orig_cpk.size());
    AppendLe64(out_patch_blob, rebuilt_cpk.size());
    out_patch_blob.insert(out_patch_blob.end(), payload.begin(), payload.end());

    // Self-check round-trip before returning success.
    Bytes verify_cpk;
    if (!ApplyMenuPatchInMemory(orig_cpk, out_patch_blob, verify_cpk) ||
        verify_cpk != rebuilt_cpk) {
        out_patch_blob.clear();
        return false;
    }

    if (out_patched_cpk != nullptr) {
        *out_patched_cpk = std::move(verify_cpk);
    }
    return true;
}

bool ApplyMenuPatchInMemory(const Bytes& orig_cpk,
                            const Bytes& patch_blob,
                            Bytes& out_patched_cpk) {
    MenuPatchPackage pkg;
    if (!pkg.LoadFromMemory(patch_blob)) {
        out_patched_cpk.clear();
        return false;
    }
    return pkg.ApplyToSysCpk(orig_cpk, out_patched_cpk);
}

}  // namespace menu_patch
