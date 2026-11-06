// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "gpk_archive.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>

namespace mgi::gpk {

namespace {

std::uint32_t rd_u32le(const Bytes& b, std::size_t off) {
    if (off + 4 > b.size())
        throw std::runtime_error("gtb/gpk: read past end of buffer");
    return static_cast<std::uint32_t>(b[off]) |
           (static_cast<std::uint32_t>(b[off + 1]) << 8) |
           (static_cast<std::uint32_t>(b[off + 2]) << 16) |
           (static_cast<std::uint32_t>(b[off + 3]) << 24);
}

void put_u32le(Bytes& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 24));
}

void put_u64le(Bytes& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

bool starts_with(const Bytes& d, const std::uint8_t* magic, std::size_t n) {
    return d.size() >= n && std::memcmp(d.data(), magic, n) == 0;
}

std::string hex_lower(std::size_t v) {
    static const char* digits = "0123456789abcdef";
    if (v == 0) return "0";
    std::string s;
    while (v) {
        s += digits[v & 0xF];
        v >>= 4;
    }
    return std::string(s.rbegin(), s.rend());
}

}  // namespace

const std::array<std::uint8_t, 16> PACKTYPE_DEFAULT = {
    'P', 'A', 'C', 'K', 'T', 'Y', 'P', 'E', '=', '8', 'A', ' ', ' ', ' ', ' ', ' '};
const std::array<std::uint8_t, 8> TRAILER_MARKER = {'o', 'v', 'e', 'r',
                                                    '2', 'G', '!', 0x00};

// ---------------------------------------------------------------------------
// GTB
// ---------------------------------------------------------------------------

Gtb read_gtb(const std::string& gtb_path) {
    const Bytes buf = read_file(gtb_path);

    Gtb gtb;
    gtb.count = rd_u32le(buf, 0);
    const std::size_t count = gtb.count;

    const std::size_t name_off_base = 4;
    const std::size_t data_off_base = name_off_base + count * 4;
    const std::size_t name_table_base = data_off_base + count * 4;

    gtb.entries.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint32_t name_off = rd_u32le(buf, name_off_base + i * 4);
        const std::uint32_t data_off = rd_u32le(buf, data_off_base + i * 4);

        const std::size_t start = name_table_base + name_off;
        std::size_t end = std::string::npos;
        for (std::size_t j = start; j < buf.size(); ++j)
            if (buf[j] == 0x00) { end = j; break; }
        // An unterminated name means the table is corrupt: fail loudly, and
        // report the RELATIVE name-table offset in decimal so it can be looked
        // up straight against the header fields.
        if (end == std::string::npos)
            throw std::runtime_error(gtb_path + ": unterminated name at offset " +
                                     std::to_string(name_off));

        gtb.entries.push_back(
            {cp932_to_utf8_replace(buf.data() + start, end - start), data_off});
    }

    // The marker is located by a substring search over the WHOLE file, not by a
    // positional check -- it does not sit at a fixed offset.
    gtb.has_64bit =
        buf.size() >= TRAILER_MARKER.size() &&
        std::search(buf.begin(), buf.end(), TRAILER_MARKER.begin(),
                    TRAILER_MARKER.end()) != buf.end();
    return gtb;
}

void write_gtb(const std::string& gtb_path, const std::vector<GtbEntry>& entries,
               bool has_64bit) {
    const std::size_t count = entries.size();

    Bytes name_table;
    std::vector<std::uint32_t> name_offsets;
    name_offsets.reserve(count);
    for (const auto& e : entries) {
        name_offsets.push_back(static_cast<std::uint32_t>(name_table.size()));
        const Bytes enc = utf8_to_cp932_replace(e.name);
        name_table.insert(name_table.end(), enc.begin(), enc.end());
        name_table.push_back(0);
    }

    Bytes out;
    put_u32le(out, static_cast<std::uint32_t>(count));
    for (std::uint32_t off : name_offsets) put_u32le(out, off);
    for (const auto& e : entries) put_u32le(out, e.offset);
    out.insert(out.end(), name_table.begin(), name_table.end());

    if (has_64bit) {
        while (out.size() % 8 != 0) out.push_back(0);
        for (const auto& e : entries) put_u64le(out, e.offset);
        out.insert(out.end(), 8, 0);
        out.insert(out.end(), TRAILER_MARKER.begin(), TRAILER_MARKER.end());
    }

    write_file(gtb_path, out);
}

// ---------------------------------------------------------------------------
// GPK entry headers
// ---------------------------------------------------------------------------

EntryHeader read_entry_header(const Bytes& gpk_buf, std::size_t offset) {
    if (offset > gpk_buf.size() || gpk_buf.size() - offset < GPK_HEADER_SIZE)
        throw std::runtime_error("truncated entry header at offset 0x" +
                                 hex_lower(offset));
    const std::uint8_t* hdr = gpk_buf.data() + offset;

    EntryHeader h;
    std::memcpy(h.type_raw.data(), hdr, 16);
    std::memcpy(h.meta.data(), hdr + 16, 16);
    h.packed_size = rd_u32le(gpk_buf, offset + 0x20);
    h.width = rd_u32le(gpk_buf, offset + 0x24);
    h.height = rd_u32le(gpk_buf, offset + 0x28);
    std::memcpy(h.packtype.data(), hdr + 0x30, 16);

    // Strip trailing 0x20 AND 0x00 in any interleaving (shipped tags mix both),
    // then ASCII-decode with every byte >= 0x80 mapped to U+FFFD.
    std::size_t n = 16;
    while (n > 0 && (h.type_raw[n - 1] == 0x20 || h.type_raw[n - 1] == 0x00)) --n;
    for (std::size_t i = 0; i < n; ++i) {
        if (h.type_raw[i] < 0x80)
            h.type_tag += static_cast<char>(h.type_raw[i]);
        else
            h.type_tag += "\xEF\xBF\xBD";  // U+FFFD
    }
    return h;
}

Archive read_archive(const std::string& gpk_path, const std::string& gtb_path) {
    Archive arc;
    arc.gtb = read_gtb(gtb_path);
    arc.gpk_buf = read_file(gpk_path);
    arc.gpk_size = arc.gpk_buf.size();

    arc.entries.reserve(arc.gtb.entries.size());
    for (const auto& ge : arc.gtb.entries) {
        EntryHeader h = read_entry_header(arc.gpk_buf, ge.offset);
        h.name = ge.name;
        h.offset = ge.offset;
        arc.entries.push_back(std::move(h));
    }
    return arc;
}

Bytes extract_entry(const Bytes& gpk_buf, std::size_t offset) {
    const EntryHeader h = read_entry_header(gpk_buf, offset);
    const std::size_t start = offset + GPK_HEADER_SIZE;
    // Truncate silently at end of buffer -- see the note on extract_entry.
    const std::size_t end = std::min(gpk_buf.size(), start + h.packed_size);
    return Bytes(gpk_buf.begin() + static_cast<std::ptrdiff_t>(start),
                 gpk_buf.begin() + static_cast<std::ptrdiff_t>(end));
}

// ---------------------------------------------------------------------------
// Payload sniffing (repack only)
// ---------------------------------------------------------------------------

std::array<std::uint8_t, 16> detect_type_tag(const Bytes& data) {
    static const std::uint8_t PNG_MAGIC[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    static const std::uint8_t BM_MAGIC[2] = {'B', 'M'};
    static const std::uint8_t JPG_MAGIC[2] = {0xFF, 0xD8};
    static const std::uint8_t GIF_MAGIC[4] = {'G', 'I', 'F', '8'};

    const char* prefix = "BIN ";
    if (starts_with(data, PNG_MAGIC, 8)) prefix = "PNG ";
    else if (starts_with(data, BM_MAGIC, 2)) prefix = "BMP ";
    else if (starts_with(data, JPG_MAGIC, 2)) prefix = "JPG ";
    else if (starts_with(data, GIF_MAGIC, 4)) prefix = "GIF ";

    std::array<std::uint8_t, 16> tag{};
    tag.fill(0x20);
    std::memcpy(tag.data(), prefix, 4);
    return tag;
}

std::pair<std::uint32_t, std::uint32_t> detect_dims(const Bytes& data) {
    static const std::uint8_t PNG4[4] = {0x89, 'P', 'N', 'G'};
    static const std::uint8_t BM_MAGIC[2] = {'B', 'M'};

    if (starts_with(data, PNG4, 4)) {
        // The 4-byte prefix is enough to commit to PNG; dimensions are a
        // BIG-ENDIAN pair in the IHDR payload.
        if (data.size() < 24) return {0, 0};
        auto be = [&](std::size_t o) {
            return (static_cast<std::uint32_t>(data[o]) << 24) |
                   (static_cast<std::uint32_t>(data[o + 1]) << 16) |
                   (static_cast<std::uint32_t>(data[o + 2]) << 8) |
                   static_cast<std::uint32_t>(data[o + 3]);
        };
        return {be(16), be(20)};
    }
    if (starts_with(data, BM_MAGIC, 2)) {
        if (data.size() < 26) return {0, 0};
        auto le_i32 = [&](std::size_t o) {
            return static_cast<std::int32_t>(
                static_cast<std::uint32_t>(data[o]) |
                (static_cast<std::uint32_t>(data[o + 1]) << 8) |
                (static_cast<std::uint32_t>(data[o + 2]) << 16) |
                (static_cast<std::uint32_t>(data[o + 3]) << 24));
        };
        const std::int64_t w = le_i32(18);
        const std::int64_t h = le_i32(22);
        return {static_cast<std::uint32_t>(w),
                static_cast<std::uint32_t>(h < 0 ? -h : h)};
    }
    return {0, 0};
}

Bytes build_entry(const EntryHeader& orig, const Bytes& payload) {
    const auto tag = detect_type_tag(payload);
    auto [width, height] = detect_dims(payload);
    if (width == 0 || height == 0) {
        width = orig.width;
        height = orig.height;
    }

    Bytes hdr(GPK_HEADER_SIZE, 0);
    std::memcpy(hdr.data(), tag.data(), 16);
    std::memcpy(hdr.data() + 16, orig.meta.data(), 16);
    const auto put = [&](std::size_t off, std::uint32_t v) {
        hdr[off] = static_cast<std::uint8_t>(v);
        hdr[off + 1] = static_cast<std::uint8_t>(v >> 8);
        hdr[off + 2] = static_cast<std::uint8_t>(v >> 16);
        hdr[off + 3] = static_cast<std::uint8_t>(v >> 24);
    };
    put(0x20, static_cast<std::uint32_t>(payload.size()));
    put(0x24, width);
    put(0x28, height);
    std::memset(hdr.data() + 0x2C, 0x20, 4);  // observed: four spaces
    std::memcpy(hdr.data() + 0x30, orig.packtype.data(), 16);

    hdr.insert(hdr.end(), payload.begin(), payload.end());
    return hdr;
}

// ---------------------------------------------------------------------------
// Repack
// ---------------------------------------------------------------------------

void repack(const std::string& gpk_in, const std::string& gtb_in,
            const std::string& gpk_out, const std::string& gtb_out,
            const std::vector<std::pair<std::string, Bytes>>& replacements) {
    const Archive arc = read_archive(gpk_in, gtb_in);
    if (arc.entries.empty())
        throw std::runtime_error(gpk_in + ": archive has no entries");

    std::map<std::string, const Bytes*> repl;
    for (const auto& [name, data] : replacements) repl[name] = &data;

    // Chunk sizes come from the NEXT entry's offset, not from packed_size:
    // sys0.gpk's "IF PACKTYPE==0..." conditional pseudo-entries store a value
    // at 0x20 that is not a byte count.
    std::vector<std::uint32_t> sorted_offs;
    sorted_offs.reserve(arc.entries.size());
    for (const auto& e : arc.entries) sorted_offs.push_back(e.offset);
    std::sort(sorted_offs.begin(), sorted_offs.end());

    // Pair each sorted offset with the next one.  Duplicate keys collapse to
    // the LAST pairing, so an offset repeated N times maps to the offset after
    // the run -- which is what makes shared-payload entries size correctly.
    std::map<std::uint32_t, std::uint32_t> next_off;
    for (std::size_t i = 0; i + 1 < sorted_offs.size(); ++i)
        next_off[sorted_offs[i]] = sorted_offs[i + 1];
    next_off[sorted_offs.back()] = static_cast<std::uint32_t>(arc.gpk_size);

    std::ofstream out(std::filesystem::u8path(gpk_out), std::ios::binary);
    if (!out) throw std::runtime_error("cannot create " + gpk_out);

    std::vector<GtbEntry> new_entries;
    new_entries.reserve(arc.entries.size());
    std::uint64_t cursor = 0;

    for (const auto& entry : arc.entries) {
        Bytes built;
        const std::uint8_t* data = nullptr;
        std::size_t size = 0;

        auto it = repl.find(entry.name);
        if (it != repl.end()) {
            built = build_entry(entry, *it->second);
            data = built.data();
            size = built.size();
        } else {
            const std::size_t start = entry.offset;
            // Every value here is already in range; the clamp keeps a
            // malformed table from reading past the buffer.
            const std::size_t end = std::min<std::size_t>(next_off.at(entry.offset),
                                                          arc.gpk_size);
            data = arc.gpk_buf.data() + start;
            size = end > start ? end - start : 0;
        }

        new_entries.push_back({entry.name, static_cast<std::uint32_t>(cursor)});
        if (size) out.write(reinterpret_cast<const char*>(data),
                            static_cast<std::streamsize>(size));
        cursor += size;
    }
    out.close();
    if (!out) throw std::runtime_error("write failed: " + gpk_out);

    write_gtb(gtb_out, new_entries, arc.gtb.has_64bit);
}

}  // namespace mgi::gpk
