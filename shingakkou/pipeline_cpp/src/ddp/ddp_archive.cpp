// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "ddp_archive.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace shin::ddp {

namespace {

std::uint32_t u32le(const Bytes& b, std::size_t off) {
    if (off + 4 > b.size())
        throw std::runtime_error("DDP3: u32 read past end of buffer");
    return static_cast<std::uint32_t>(b[off]) |
           (static_cast<std::uint32_t>(b[off + 1]) << 8) |
           (static_cast<std::uint32_t>(b[off + 2]) << 16) |
           (static_cast<std::uint32_t>(b[off + 3]) << 24);
}

// Renders raw bytes as b'...' with \xNN escapes, the form the bad-magic error
// message quotes the header in.
std::string bytes_repr(const std::uint8_t* p, std::size_t n) {
    std::string out = "b'";
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t c = p[i];
        if (c == '\\') out += "\\\\";
        else if (c == '\'') out += "\\'";
        else if (c == '\t') out += "\\t";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c >= 0x20 && c < 0x7F) out += static_cast<char>(c);
        else {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\x%02x", c);
            out += buf;
        }
    }
    return out + "'";
}

// UTF-16LE decode, then strip trailing U+0000 ONLY -- not whitespace.
// trim_right here would eat a trailing space in a script name (no current name
// has one, but the two are not interchangeable).
std::string utf16le_name(const std::uint8_t* p, std::size_t n) {
    std::vector<std::uint16_t> units;
    units.reserve(n / 2);
    for (std::size_t i = 0; i + 1 < n; i += 2)
        units.push_back(static_cast<std::uint16_t>(p[i] | (p[i + 1] << 8)));
    while (!units.empty() && units.back() == 0) units.pop_back();

    std::string out;
    for (std::size_t i = 0; i < units.size(); ++i) {
        // Unlike the string walk in the extractor (one codepoint per raw code
        // unit), archive names are decoded as real UTF-16: surrogate pairs
        // combine.  All 91 names are ASCII, so the branch never fires here --
        // it is present so the two decoders cannot be confused for each other.
        const std::uint16_t u = units[i];
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < units.size() &&
            units[i + 1] >= 0xDC00 && units[i + 1] <= 0xDFFF) {
            const char32_t cp =
                0x10000 + ((static_cast<char32_t>(u) - 0xD800) << 10) +
                (static_cast<char32_t>(units[++i]) - 0xDC00);
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            append_utf16_unit_as_utf8(u, out);
        }
    }
    return out;
}

}  // namespace

std::vector<ScriptEntry> parse_ddp3(const Bytes& data) {
    if (data.size() < 4 || data[0] != 'D' || data[1] != 'D' || data[2] != 'P' ||
        data[3] != '3')
        throw std::runtime_error(
            "Not a DDP3 archive (magic: " +
            bytes_repr(data.data(), data.size() < 4 ? data.size() : 4) + ")");

    const std::uint32_t num_outer = u32le(data, 4);
    std::vector<ScriptEntry> scripts;

    for (std::uint32_t outer_idx = 0; outer_idx < num_outer; ++outer_idx) {
        const std::size_t entry_off = 32 + static_cast<std::size_t>(outer_idx) * 8;
        const std::uint32_t inner_size = u32le(data, entry_off);
        const std::uint32_t inner_offset = u32le(data, entry_off + 4);

        // Clamp rather than fail: a section pointing past EOF yields an empty
        // chunk and is skipped by the loop below.
        const std::size_t begin = std::min<std::size_t>(inner_offset, data.size());
        const std::size_t end =
            std::min<std::size_t>(static_cast<std::size_t>(inner_offset) + inner_size,
                                  data.size());
        const std::uint8_t* chunk = data.data() + begin;
        const std::size_t chunk_len = end > begin ? end - begin : 0;

        std::size_t pos = 0;
        while (pos < chunk_len) {
            const std::size_t entry_len = chunk[pos];
            if (entry_len == 0) break;
            if (pos + entry_len > chunk_len) break;
            const std::uint8_t* e = chunk + pos;
            if (entry_len < 13)
                throw std::runtime_error("DDP3: entry too short for its header");

            ScriptEntry s;
            s.name = entry_len > 17 ? utf16le_name(e + 17, entry_len - 17) : std::string();
            s.offset = static_cast<std::uint32_t>(e[1]) |
                       (static_cast<std::uint32_t>(e[2]) << 8) |
                       (static_cast<std::uint32_t>(e[3]) << 16) |
                       (static_cast<std::uint32_t>(e[4]) << 24);
            s.decomp_size = static_cast<std::uint32_t>(e[5]) |
                            (static_cast<std::uint32_t>(e[6]) << 8) |
                            (static_cast<std::uint32_t>(e[7]) << 16) |
                            (static_cast<std::uint32_t>(e[8]) << 24);
            s.comp_size = static_cast<std::uint32_t>(e[9]) |
                          (static_cast<std::uint32_t>(e[10]) << 8) |
                          (static_cast<std::uint32_t>(e[11]) << 16) |
                          (static_cast<std::uint32_t>(e[12]) << 24);
            // e[13..17) is padding; nothing in the pipeline reads it.
            scripts.push_back(std::move(s));
            pos += entry_len;
        }
    }

    return scripts;
}

Bytes shs_decompress(const Bytes& src, std::uint32_t decomp_size) {
    Bytes output(decomp_size, 0);
    const std::int64_t out_size = static_cast<std::int64_t>(decomp_size);
    const std::int64_t src_size = static_cast<std::int64_t>(src.size());
    std::int64_t dst = 0, si = 0;

    // The shipped archive never trips any of these, but a silent OOB read here
    // would corrupt a whole script, so they are hard failures.
    auto need = [&](std::int64_t k) {
        if (si + k > src_size) throw std::runtime_error("Shs: control read past end of input");
    };

    while (dst < out_size && si < src_size) {
        const std::uint8_t ctl = src[static_cast<std::size_t>(si)];
        ++si;
        std::int64_t count = 0;

        if (ctl < 32) {
            // ---- literal run ----
            if (ctl == 0x1D) {
                need(1);
                count = src[static_cast<std::size_t>(si)] + 0x1E;
                si += 1;
            } else if (ctl == 0x1E) {
                need(2);
                count = ((src[static_cast<std::size_t>(si)] << 8) |
                         src[static_cast<std::size_t>(si) + 1]) + 0x11E;
                si += 2;
            } else if (ctl == 0x1F) {
                // BIG-endian u32 with no bias.  Unused by the shipped archive,
                // but it is exactly what ddp_archive.shs_wrap emits, so a
                // repack round-trip depends on it.
                need(4);
                count = (static_cast<std::int64_t>(src[static_cast<std::size_t>(si)]) << 24) |
                        (static_cast<std::int64_t>(src[static_cast<std::size_t>(si) + 1]) << 16) |
                        (static_cast<std::int64_t>(src[static_cast<std::size_t>(si) + 2]) << 8) |
                        static_cast<std::int64_t>(src[static_cast<std::size_t>(si) + 3]);
                si += 4;
            } else {
                count = ctl + 1;
            }

            count = std::min(count, out_size - dst);
            // A literal run overrunning the input is corruption, not a short
            // copy; measured 0 occurrences over all 91 scripts.
            if (si + count > src_size)
                throw std::runtime_error("Shs: literal run past end of input");
            for (std::int64_t i = 0; i < count; ++i)
                output[static_cast<std::size_t>(dst + i)] =
                    src[static_cast<std::size_t>(si + i)];
            si += count;
        } else {
            // ---- back-reference ----
            std::int64_t offset = 0;
            if (!(ctl & 0x80)) {
                if ((ctl & 0x60) == 0x20) {
                    offset = (ctl >> 2) & 7;
                    count = ctl & 3;
                } else if ((ctl & 0x60) == 0x40) {
                    need(1);
                    offset = src[static_cast<std::size_t>(si)];
                    si += 1;
                    count = (ctl & 0x1F) + 4;
                } else {
                    need(1);
                    offset = src[static_cast<std::size_t>(si)];
                    si += 1;
                    offset |= static_cast<std::int64_t>(ctl & 0x1F) << 8;
                    need(1);
                    const std::uint8_t ctl2 = src[static_cast<std::size_t>(si)];
                    si += 1;
                    if (ctl2 == 0xFE) {
                        need(2);
                        count = ((src[static_cast<std::size_t>(si)] << 8) |
                                 src[static_cast<std::size_t>(si) + 1]) + 0x102;
                        si += 2;
                    } else if (ctl2 == 0xFF) {
                        need(4);
                        count =
                            (static_cast<std::int64_t>(src[static_cast<std::size_t>(si)]) << 24) |
                            (static_cast<std::int64_t>(src[static_cast<std::size_t>(si) + 1]) << 16) |
                            (static_cast<std::int64_t>(src[static_cast<std::size_t>(si) + 2]) << 8) |
                            static_cast<std::int64_t>(src[static_cast<std::size_t>(si) + 3]);
                        si += 4;
                    } else {
                        count = ctl2 + 4;
                    }
                }
            } else {
                count = (ctl >> 5) & 3;
                need(1);
                offset = (static_cast<std::int64_t>(ctl & 0x1F) << 8) |
                         src[static_cast<std::size_t>(si)];
                si += 1;
            }

            count += 3;
            offset += 1;
            count = std::min(count, out_size - dst);
            const std::int64_t src_pos = dst - offset;
            // A back-reference before the output start is corruption, not a
            // wrap to the end of the buffer; measured 0 occurrences.
            if (src_pos < 0) throw std::runtime_error("Shs: back-reference before start");
            // Forward, byte at a time: 1,345 real back-references read bytes
            // this very copy just wrote (RLE), so memcpy/memmove are wrong.
            for (std::int64_t i = 0; i < count; ++i)
                output[static_cast<std::size_t>(dst + i)] =
                    output[static_cast<std::size_t>(src_pos + i)];
        }

        dst += count;
    }

    output.resize(static_cast<std::size_t>(dst));
    return output;
}

Bytes decrypt_hxb(const Bytes& data) {
    if (data.size() < 0x11) return data;
    const std::uint32_t length = (static_cast<std::uint32_t>(data[8]) << 16) |
                                 (static_cast<std::uint32_t>(data[9]) << 8) |
                                 static_cast<std::uint32_t>(data[10]);
    // uint32 throughout: the multiply's low 32 bits are the key, and widening
    // it (or letting it overflow a signed type) silently changes every byte.
    const std::uint32_t key =
        (((length << 5) ^ 0xA5u) * (length + 0x6F349u)) ^ 0x34A9B129u;
    const std::uint8_t kb[4] = {
        static_cast<std::uint8_t>(key & 0xFF), static_cast<std::uint8_t>((key >> 8) & 0xFF),
        static_cast<std::uint8_t>((key >> 16) & 0xFF),
        static_cast<std::uint8_t>((key >> 24) & 0xFF)};

    Bytes dec = data;
    for (std::size_t i = 0x10; i < dec.size(); ++i) dec[i] ^= kb[i & 3];
    return dec;
}

// ---------------------------------------------------------------------------
// CG-archive repacker
// ---------------------------------------------------------------------------

namespace {

constexpr std::size_t kHeaderSize = 0x20;
constexpr std::size_t kDdp2IndexEntrySize = 16;
constexpr std::size_t kDdp3SectionSize = 8;
constexpr std::size_t kDdp3EntryHeaderSize = 17;  // size byte + 4 u32 fields

void put_u32le(Bytes& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
}

// os.path.splitext(os.path.basename(path))[0]
std::string archive_stem(const std::string& path) {
    const std::size_t sep = path.find_last_of("\\/");
    const std::string base = sep == std::string::npos ? path : path.substr(sep + 1);
    const std::size_t dot = base.find_last_of('.');
    return (dot == std::string::npos || dot == 0) ? base : base.substr(0, dot);
}

// "<stem>#00042" -- str(i).zfill(5).
std::string ddp2_entry_name(const std::string& stem, std::size_t i) {
    std::string n = std::to_string(i);
    if (n.size() < 5) n.insert(0, 5 - n.size(), '0');
    return stem + "#" + n;
}

// The first `n` bytes of a file (fewer if it is shorter).
//
// These archives run to 800 MB, so nothing here ever loads a whole one: the
// header, the section table and every entry block all live below the data
// section, and the payloads are streamed.  Reading the file whole cost an
// access violation on sin_cgev.dat -- source + payload copies + output was
// three simultaneous copies of the archive.
Bytes read_head(const std::string& path, std::size_t n) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    Bytes buf(n);
    f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(n));
    buf.resize(static_cast<std::size_t>(f.gcount()));
    return buf;
}

Bytes read_magic(const std::string& path) { return read_head(path, 4); }

const Bytes* find_replacement(const Replacements& reps, const std::string& name) {
    for (const auto& [k, v] : reps)
        if (k == name) return &v;
    return nullptr;
}

// Copy `size` bytes from `in` at `offset` to `out`, clamping at EOF.  Returns
// how many bytes were actually written, which is what the new index has to
// record.
std::uint32_t copy_payload(std::ifstream& in, std::ofstream& out, std::uint32_t offset,
                           std::uint32_t size) {
    in.clear();
    in.seekg(static_cast<std::streamoff>(offset));
    if (!in) {
        in.clear();
        return 0;  // seek past EOF -> read() returns b''
    }
    constexpr std::size_t kChunk = 1 << 20;
    std::vector<char> buf(std::min<std::size_t>(kChunk, size ? size : 1));
    std::uint32_t left = size, written = 0;
    while (left > 0) {
        const std::streamsize want =
            static_cast<std::streamsize>(std::min<std::size_t>(buf.size(), left));
        in.read(buf.data(), want);
        const std::streamsize got = in.gcount();
        if (got <= 0) break;
        out.write(buf.data(), got);
        written += static_cast<std::uint32_t>(got);
        left -= static_cast<std::uint32_t>(got);
    }
    in.clear();
    return written;
}

// The byte count an unreplaced entry keeps: `pck`, or `unp` when pck is 0.
std::uint32_t stored_size(std::uint32_t unp, std::uint32_t pck) {
    return pck != 0 ? pck : unp;
}

// How many of those bytes the file actually has -- a read past EOF is clamped,
// and the offsets in the new index have to account for the shortfall.
std::uint32_t available_at(std::ifstream& in, std::uint32_t offset, std::uint32_t size) {
    in.clear();
    in.seekg(0, std::ios::end);
    const std::uint64_t file_size = static_cast<std::uint64_t>(in.tellg());
    in.clear();
    if (offset >= file_size) return 0;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(size, file_size - offset));
}

struct Ddp2Index {
    Bytes header;  // the raw 0x20 bytes
    std::vector<ArchiveEntry> entries;
};

Ddp2Index read_ddp2_index(const Bytes& data, const std::string& stem) {
    Ddp2Index idx;
    if (data.size() < kHeaderSize) throw std::runtime_error("DDP2: file shorter than header");
    idx.header.assign(data.begin(), data.begin() + kHeaderSize);
    const std::uint32_t count = u32le(data, 4);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t off = kHeaderSize + static_cast<std::size_t>(i) * kDdp2IndexEntrySize;
        idx.entries.push_back({ddp2_entry_name(stem, i), u32le(data, off), u32le(data, off + 4),
                               u32le(data, off + 8)});
    }
    return idx;
}

// A DDP3 entry, with everything repack has to write back verbatim.
struct Ddp3Entry {
    std::uint8_t e_sz = 0;
    std::uint32_t offset = 0;
    std::uint32_t unpacked = 0;
    std::uint32_t packed = 0;
    Bytes name_bytes;  // exact UTF-16LE bytes, written back unchanged
    std::string name;
};

struct Ddp3Section {
    std::uint32_t block_size = 0;
    std::uint32_t abs_offset = 0;
    std::vector<Ddp3Entry> entries;
};

struct Ddp3Index {
    Bytes header;
    std::vector<Ddp3Section> sections;
};

Ddp3Index read_ddp3_index(const Bytes& data) {
    Ddp3Index idx;
    if (data.size() < kHeaderSize) throw std::runtime_error("DDP3: file shorter than header");
    idx.header.assign(data.begin(), data.begin() + kHeaderSize);
    const std::uint32_t count = u32le(data, 4);

    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t off = kHeaderSize + static_cast<std::size_t>(i) * kDdp3SectionSize;
        Ddp3Section sec;
        sec.block_size = u32le(data, off);
        sec.abs_offset = u32le(data, off + 4);

        if (sec.block_size > 0) {
            std::size_t pos = sec.abs_offset;
            std::int64_t remaining = sec.block_size;
            while (remaining > 0) {
                if (pos >= data.size()) break;  // nothing left to read
                const std::uint8_t e_sz = data[pos];
                if (e_sz < kDdp3EntryHeaderSize) break;
                if (pos + e_sz > data.size()) break;
                Ddp3Entry e;
                e.e_sz = e_sz;
                e.offset = u32le(data, pos + 1);
                e.unpacked = u32le(data, pos + 5);
                e.packed = u32le(data, pos + 9);
                // pos+13..17 is padding, always written back as zeroes.
                e.name_bytes.assign(data.begin() + static_cast<std::ptrdiff_t>(pos + 17),
                                    data.begin() + static_cast<std::ptrdiff_t>(pos + e_sz));
                e.name = utf16le_name(e.name_bytes.data(), e.name_bytes.size());
                sec.entries.push_back(std::move(e));
                pos += e_sz;
                remaining -= e_sz;
            }
        }
        idx.sections.push_back(std::move(sec));
    }
    return idx;
}

}  // namespace

Bytes shs_wrap(const Bytes& data) {
    Bytes out;
    out.reserve(data.size() + 5);
    out.push_back(0x1F);
    const std::uint32_t n = static_cast<std::uint32_t>(data.size());
    out.push_back(static_cast<std::uint8_t>((n >> 24) & 0xFF));  // big-endian
    out.push_back(static_cast<std::uint8_t>((n >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((n >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(n & 0xFF));
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

namespace {

// Enough of the file to cover the header, the index/section table and (DDP3)
// every entry block -- all of which sit below the data section.
Bytes read_ddp2_head(const std::string& path) {
    const Bytes header = read_head(path, kHeaderSize);
    if (header.size() < kHeaderSize) throw std::runtime_error("DDP2: file shorter than header");
    const std::uint32_t count = u32le(header, 4);
    return read_head(path, kHeaderSize + static_cast<std::size_t>(count) * kDdp2IndexEntrySize);
}

Bytes read_ddp3_head(const std::string& path) {
    const Bytes header = read_head(path, kHeaderSize);
    if (header.size() < kHeaderSize) throw std::runtime_error("DDP3: file shorter than header");
    const std::uint32_t field8 = u32le(header, 8);  // absolute start of the data
    return read_head(path, field8);
}

}  // namespace

std::vector<ArchiveEntry> list_ddp2(const std::string& path) {
    return read_ddp2_index(read_ddp2_head(path), archive_stem(path)).entries;
}

std::vector<ArchiveEntry> list_ddp3(const std::string& path) {
    std::vector<ArchiveEntry> out;
    for (const auto& sec : read_ddp3_index(read_ddp3_head(path)).sections)
        for (const auto& e : sec.entries)
            out.push_back({e.name, e.offset, e.unpacked, e.packed});
    return out;
}

std::vector<ArchiveEntry> list_archive(const std::string& path) {
    const Bytes magic = read_magic(path);
    if (magic.size() >= 4 && std::equal(magic.begin(), magic.begin() + 4, "DDP2"))
        return list_ddp2(path);
    if (magic.size() >= 4 && std::equal(magic.begin(), magic.begin() + 4, "DDP3"))
        return list_ddp3(path);
    throw std::runtime_error("Not a DDP archive: magic=" +
                             bytes_repr(magic.data(), magic.size()));
}

void repack_ddp2(const std::string& in_path, const std::string& out_path,
                 const Replacements& replacements) {
    const Ddp2Index idx = read_ddp2_index(read_ddp2_head(in_path), archive_stem(in_path));
    const std::size_t count = idx.entries.size();
    const std::uint32_t data_start =
        static_cast<std::uint32_t>(kHeaderSize + count * kDdp2IndexEntrySize);

    std::ifstream in(std::filesystem::u8path(in_path), std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + in_path);

    // Pass 1: the index needs every payload's final size, and an unreplaced
    // entry truncated at EOF stores fewer bytes than its header claims -- so
    // measure first, write the index, then stream the data.
    struct Plan {
        std::uint32_t offset;
        std::uint32_t unpacked;
        std::uint32_t packed;
        const Bytes* replacement;  // null -> copy from the source
        std::uint32_t stored;      // bytes actually written
    };
    std::vector<Plan> plan;
    plan.reserve(count);
    std::uint32_t cursor = data_start;
    for (const auto& e : idx.entries) {
        Plan p{cursor, e.unpacked, e.packed, nullptr, 0};
        if (const Bytes* rep = find_replacement(replacements, e.name)) {
            p.replacement = rep;
            p.unpacked = static_cast<std::uint32_t>(rep->size());
            p.packed = static_cast<std::uint32_t>(rep->size() + 5);  // shs_wrap overhead
            p.stored = p.packed;
        } else {
            p.stored = available_at(in, e.offset, stored_size(e.unpacked, e.packed));
        }
        cursor += p.stored;
        plan.push_back(p);
    }

    std::ofstream out(std::filesystem::u8path(out_path), std::ios::binary);
    if (!out) throw std::runtime_error("cannot create " + out_path);

    Bytes head;
    head.insert(head.end(), idx.header.begin(), idx.header.begin() + 4);  // magic
    put_u32le(head, static_cast<std::uint32_t>(count));
    head.insert(head.end(), idx.header.begin() + 8, idx.header.end());  // field8 + padding
    for (const auto& p : plan) {
        put_u32le(head, p.offset);
        put_u32le(head, p.unpacked);
        put_u32le(head, p.packed);
        put_u32le(head, 0);
    }
    out.write(reinterpret_cast<const char*>(head.data()),
              static_cast<std::streamsize>(head.size()));

    for (std::size_t i = 0; i < plan.size(); ++i) {
        if (plan[i].replacement) {
            const Bytes wrapped = shs_wrap(*plan[i].replacement);
            out.write(reinterpret_cast<const char*>(wrapped.data()),
                      static_cast<std::streamsize>(wrapped.size()));
        } else {
            copy_payload(in, out, idx.entries[i].offset,
                         stored_size(idx.entries[i].unpacked, idx.entries[i].packed));
        }
    }
    if (!out) throw std::runtime_error("write failed: " + out_path);
}

void repack_ddp3(const std::string& in_path, const std::string& out_path,
                 const Replacements& replacements) {
    const Bytes head = read_ddp3_head(in_path);
    const Ddp3Index idx = read_ddp3_index(head);
    const std::uint32_t count = u32le(head, 4);
    const std::uint32_t field8 = u32le(head, 8);  // absolute start of the data section

    std::ifstream in(std::filesystem::u8path(in_path), std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + in_path);

    // Flatten in section/entry order and assign new data offsets from field8.
    struct Plan {
        std::uint32_t offset;
        std::uint32_t unpacked;
        std::uint32_t packed;
        const Bytes* replacement;  // null -> copy from the source
        std::uint32_t src_offset;
        std::uint32_t stored;
    };
    std::vector<Plan> plan;
    std::uint32_t cursor = field8;
    for (const auto& sec : idx.sections) {
        for (const auto& e : sec.entries) {
            Plan p{cursor, e.unpacked, e.packed, nullptr, e.offset, 0};
            if (const Bytes* rep = find_replacement(replacements, e.name)) {
                p.replacement = rep;
                p.unpacked = static_cast<std::uint32_t>(rep->size());
                p.packed = static_cast<std::uint32_t>(rep->size() + 5);  // shs_wrap overhead
                p.stored = p.packed;
            } else {
                p.stored = available_at(in, e.offset, stored_size(e.unpacked, e.packed));
            }
            cursor += p.stored;
            plan.push_back(p);
        }
    }

    // Everything below the data section is small enough to build in memory.
    Bytes out;
    out.insert(out.end(), head.begin(), head.begin() + 4);  // magic
    put_u32le(out, count);
    put_u32le(out, field8);
    out.insert(out.end(), head.begin() + 12, head.begin() + kHeaderSize);

    for (const auto& sec : idx.sections) {
        put_u32le(out, sec.block_size);
        put_u32le(out, sec.abs_offset);
    }

    std::size_t entry_idx = 0;
    for (const auto& sec : idx.sections) {
        std::uint32_t written = 0;
        for (const auto& e : sec.entries) {
            const Plan& p = plan[entry_idx++];
            out.push_back(e.e_sz);
            put_u32le(out, p.offset);
            put_u32le(out, p.unpacked);
            put_u32le(out, p.packed);
            put_u32le(out, 0);  // padding is always zero
            out.insert(out.end(), e.name_bytes.begin(), e.name_bytes.end());
            written += e.e_sz;
        }
        // Pad the block back to its original size (keeps sentinels/alignment).
        if (written < sec.block_size) out.insert(out.end(), sec.block_size - written, 0);
    }
    if (out.size() < field8) out.insert(out.end(), field8 - out.size(), 0);

    std::ofstream fout(std::filesystem::u8path(out_path), std::ios::binary);
    if (!fout) throw std::runtime_error("cannot create " + out_path);
    fout.write(reinterpret_cast<const char*>(out.data()),
               static_cast<std::streamsize>(out.size()));

    for (const auto& p : plan) {
        if (p.replacement) {
            const Bytes wrapped = shs_wrap(*p.replacement);
            fout.write(reinterpret_cast<const char*>(wrapped.data()),
                       static_cast<std::streamsize>(wrapped.size()));
        } else {
            copy_payload(in, fout, p.src_offset, p.stored);
        }
    }
    if (!fout) throw std::runtime_error("write failed: " + out_path);
}

void repack(const std::string& in_path, const std::string& out_path,
            const Replacements& replacements) {
    const Bytes magic = read_magic(in_path);
    if (magic.size() >= 4 && std::equal(magic.begin(), magic.begin() + 4, "DDP2"))
        repack_ddp2(in_path, out_path, replacements);
    else if (magic.size() >= 4 && std::equal(magic.begin(), magic.begin() + 4, "DDP3"))
        repack_ddp3(in_path, out_path, replacements);
    else
        throw std::runtime_error("Not a DDP archive: magic=" +
                                 bytes_repr(magic.data(), magic.size()));
}

}  // namespace shin::ddp
