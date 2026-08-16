// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "yuris/ypf_archive.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include <zlib.h>

namespace frat::yuris {

namespace {

std::uint32_t rd_u32(const Bytes& b, std::size_t off) {
    return static_cast<std::uint32_t>(b[off]) |
           (static_cast<std::uint32_t>(b[off + 1]) << 8) |
           (static_cast<std::uint32_t>(b[off + 2]) << 16) |
           (static_cast<std::uint32_t>(b[off + 3]) << 24);
}

std::uint64_t rd_u64(const Bytes& b, std::size_t off) {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | b[off + static_cast<std::size_t>(i)];
    return v;
}

}  // namespace

std::string basename(const std::string& entry_name) {
    std::string s = entry_name;
    if (const std::size_t p = s.rfind('\\'); p != std::string::npos) s = s.substr(p + 1);
    if (const std::size_t p = s.rfind('/'); p != std::string::npos) s = s.substr(p + 1);
    return s;
}

std::vector<YpfEntry> parse_ypf_index_bytes(const Bytes& file, const std::string& path) {
    if (file.size() < 32 || std::memcmp(file.data(), "YPF\0", 4) != 0)
        throw std::runtime_error("not a YPF: " + path);
    const std::uint32_t file_count = rd_u32(file, 8);
    const std::uint32_t idx_size = rd_u32(file, 12);

    // Clamp the index slice at EOF, so a truncated archive yields a short
    // index rather than an exception.
    const std::size_t idx_begin = 32;
    const std::size_t idx_end = std::min<std::size_t>(file.size(), idx_begin + idx_size);
    const Bytes idx(file.begin() + static_cast<std::ptrdiff_t>(idx_begin),
                    file.begin() + static_cast<std::ptrdiff_t>(idx_end));

    std::vector<YpfEntry> entries;
    std::size_t off = 0;
    for (std::uint32_t k = 0; k < file_count; ++k) {
        if (off + 5 > idx.size()) break;
        off += 4;  // name_hash (skipped)
        const std::uint8_t name_size_raw = idx[off];
        ++off;

        // Auto-detect the real name length by probing for a valid footer.  A
        // name byte XOR 0xFF is 0x81-0xDF for any printable ASCII, i.e. always
        // > 20, so the `t > 20` guard can never stop inside a name.
        const int hinted = name_size_raw ^ YPF_NAME_XOR_KEY;
        const long long room = static_cast<long long>(idx.size()) -
                               static_cast<long long>(off) - 22;
        const long long max_try = std::min<long long>(hinted + 8, room);
        int name_len = 0;
        for (long long n = 1; n <= max_try; ++n) {
            const std::size_t tail = off + static_cast<std::size_t>(n);
            if (tail + 22 > idx.size()) break;
            const std::uint8_t t = idx[tail];
            const std::uint8_t c = idx[tail + 1];
            if (t > 20 || c > 1) continue;
            const std::uint32_t raw = rd_u32(idx, tail + 2);
            const std::uint32_t packed = rd_u32(idx, tail + 6);
            bool printable = true;
            for (std::size_t j = 0; j < static_cast<std::size_t>(n); ++j) {
                const std::uint8_t d = idx[off + j] ^ YPF_NAME_XOR_KEY;
                if (d < 0x20 || d >= 0x7F) { printable = false; break; }
            }
            if (!printable) continue;
            if ((c == 0 && raw == packed) || c == 1) {
                name_len = static_cast<int>(n);
                break;
            }
        }
        if (name_len == 0) name_len = hinted;
        // The hinted fallback is unvalidated, so it can point past the end of
        // the index; stop rather than read a footer out of bounds.
        if (off + static_cast<std::size_t>(name_len) + 22 > idx.size()) break;

        Bytes name_bytes(static_cast<std::size_t>(name_len));
        for (std::size_t j = 0; j < name_bytes.size(); ++j)
            name_bytes[j] = static_cast<std::uint8_t>(idx[off + j] ^ YPF_NAME_XOR_KEY);
        YpfEntry e;
        e.name = cp932_to_utf8_replace(name_bytes.data(), name_bytes.size());
        off += static_cast<std::size_t>(name_len);
        e.type = idx[off];
        ++off;
        e.compressed = idx[off];
        ++off;
        e.raw_size = rd_u32(idx, off);
        e.packed_size = rd_u32(idx, off + 4);
        off += 8;
        e.data_offset = rd_u64(idx, off);
        off += 8;
        off += 4;  // data_hash (skipped)
        entries.push_back(std::move(e));
    }
    return entries;
}

std::vector<YpfEntry> parse_ypf_index(const std::string& path) {
    return parse_ypf_index_bytes(read_file(path), path);
}

Bytes decrypt_ystb(const Bytes& blob) {
    if (blob.size() <= 0x20 || std::memcmp(blob.data(), "YSTB", 4) != 0) return blob;
    const std::uint32_t csz = rd_u32(blob, 12);
    const std::uint32_t asz = rd_u32(blob, 16);
    const std::uint32_t rsz = rd_u32(blob, 20);
    const std::uint32_t osz = rd_u32(blob, 24);
    // Malformed or a different YSTB variant: leave it alone.
    if (0x20ull + csz + asz + rsz + osz != blob.size()) return blob;

    Bytes buf = blob;
    std::size_t off = 0x20;
    for (const std::uint32_t sz : {csz, asz, rsz, osz}) {
        const std::size_t end4 = off + (sz & ~3u);
        for (std::size_t i = off; i < end4; i += 4) {
            const std::uint32_t w = rd_u32(buf, i) ^ YSTB_SCRIPT_KEY;
            buf[i] = static_cast<std::uint8_t>(w);
            buf[i + 1] = static_cast<std::uint8_t>(w >> 8);
            buf[i + 2] = static_cast<std::uint8_t>(w >> 16);
            buf[i + 3] = static_cast<std::uint8_t>(w >> 24);
        }
        const std::uint32_t rem = sz & 3u;
        std::uint32_t k = YSTB_SCRIPT_KEY;
        for (std::uint32_t j = 0; j < rem; ++j) {
            buf[end4 + j] ^= static_cast<std::uint8_t>(k & 0xFF);
            k >>= 8;
        }
        off += sz;
    }
    return buf;
}

namespace {

// Entry bodies are a zlib STREAM (2-byte header), not raw deflate.
// raw_size is the index's claim, so grow instead of trusting it blindly.
Bytes zlib_inflate(const Bytes& in, std::uint32_t hint) {
    Bytes out(std::max<std::size_t>(hint, in.size() * 4 + 64));
    for (;;) {
        uLongf dest_len = static_cast<uLongf>(out.size());
        const int rc = ::uncompress(out.data(), &dest_len, in.data(),
                                    static_cast<uLong>(in.size()));
        if (rc == Z_OK) {
            out.resize(dest_len);
            return out;
        }
        if (rc != Z_BUF_ERROR) throw std::runtime_error("zlib inflate failed");
        out.resize(out.size() * 2 + 1024);
    }
}

}  // namespace

Bytes read_entry(const Bytes& file, const YpfEntry& entry) {
    Bytes blob;
    if (entry.data_offset < file.size()) {
        const std::size_t begin = static_cast<std::size_t>(entry.data_offset);
        const std::size_t end = std::min<std::size_t>(file.size(), begin + entry.packed_size);
        blob.assign(file.begin() + static_cast<std::ptrdiff_t>(begin),
                    file.begin() + static_cast<std::ptrdiff_t>(end));
    }
    if (entry.compressed) blob = zlib_inflate(blob, entry.raw_size);
    if (blob.size() >= 4 && std::memcmp(blob.data(), "YSTB", 4) == 0)
        blob = decrypt_ystb(blob);
    return blob;
}

}  // namespace frat::yuris
