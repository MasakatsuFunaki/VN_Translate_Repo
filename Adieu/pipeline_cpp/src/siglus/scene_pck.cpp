// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "scene_pck.h"

#include <stdexcept>

namespace adi::siglus {

namespace {

// Little-endian u32 read.  An out-of-range offset means the header itself is
// bad, so refuse loudly rather than read past the buffer.
std::uint32_t u32le(const Bytes& data, std::uint64_t off) {
    if (off + 4 > data.size())
        throw std::runtime_error("unpack_requires a buffer of at least 4 bytes at offset " +
                                 std::to_string(off));
    return static_cast<std::uint32_t>(data[off]) |
           (static_cast<std::uint32_t>(data[off + 1]) << 8) |
           (static_cast<std::uint32_t>(data[off + 2]) << 16) |
           (static_cast<std::uint32_t>(data[off + 3]) << 24);
}

}  // namespace

Bytes slice_clamped(const Bytes& data, std::uint64_t start, std::uint64_t size) {
    const std::uint64_t n = data.size();
    if (start >= n) return {};
    const std::uint64_t end = (start + size > n) ? n : start + size;
    return Bytes(data.begin() + static_cast<std::ptrdiff_t>(start),
                 data.begin() + static_cast<std::ptrdiff_t>(end));
}

Header parse_header(const Bytes& data) {
    Header h;
    h.header_size = u32le(data, 0);
    for (int i = 0; i < SECTION_COUNT; ++i) {
        h.sections[i].offset = u32le(data, 4 + static_cast<std::uint64_t>(i) * 8);
        h.sections[i].count = u32le(data, 4 + static_cast<std::uint64_t>(i) * 8 + 4);
    }
    return h;
}

std::vector<std::string> read_name_table(const Bytes& data, std::uint32_t idx_off,
                                         std::uint32_t idx_cnt, std::uint32_t blob_off) {
    std::vector<std::string> names;
    names.reserve(idx_cnt);
    for (std::uint32_t i = 0; i < idx_cnt; ++i) {
        const std::uint64_t entry = static_cast<std::uint64_t>(idx_off) + static_cast<std::uint64_t>(i) * 8;
        const std::uint32_t woff = u32le(data, entry);
        const std::uint32_t wlen = u32le(data, entry + 4);
        // 64-bit math: woff/wlen are u32 straight out of the file, so
        // (woff + wlen) * 2 must not be evaluated in 32 bits.
        const std::uint64_t start = static_cast<std::uint64_t>(blob_off) + static_cast<std::uint64_t>(woff) * 2;
        const std::uint64_t end =
            static_cast<std::uint64_t>(blob_off) +
            (static_cast<std::uint64_t>(woff) + static_cast<std::uint64_t>(wlen)) * 2;
        const Bytes raw = slice_clamped(data, start, end > start ? end - start : 0);
        names.push_back(utf16le_decode_replace(raw.data(), raw.size()));
    }
    return names;
}

std::vector<ScriptMeta> read_script_meta(const Bytes& data, const Header& h,
                                         const std::vector<std::string>& script_names) {
    const std::uint32_t idx2_off = h.sections[SEC_SCRIPT_DATA_IDX].offset;
    const std::uint32_t data_base = h.sections[SEC_SCRIPT_KEY].offset;
    const std::uint32_t count = h.sections[SEC_SCRIPT_NAME_IDX].count;

    std::vector<ScriptMeta> out;
    out.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint64_t entry = static_cast<std::uint64_t>(idx2_off) + static_cast<std::uint64_t>(i) * 8;
        ScriptMeta m;
        m.name = i < script_names.size() ? script_names[i] : std::string();
        m.rel_offset = u32le(data, entry);
        m.size = u32le(data, entry + 4);
        m.abs_offset = static_cast<std::uint64_t>(data_base) + m.rel_offset;
        out.push_back(std::move(m));
    }
    return out;
}

}  // namespace adi::siglus
