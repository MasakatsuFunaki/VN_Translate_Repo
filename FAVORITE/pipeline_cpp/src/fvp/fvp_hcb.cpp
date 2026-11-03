// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "fvp_hcb.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <stdexcept>

namespace fav::fvp {

namespace {

// Opcode -> operand type list.  These widths are what make the walk
// self-synchronising, so the table is exhaustive for the opcodes FVP emits;
// an opcode missing from it is treated as format drift, not skipped.
const std::map<std::uint8_t, std::vector<Arg>>& opcode_table() {
    static const std::map<std::uint8_t, std::vector<Arg>> table = {
        {0x01, {Arg::B, Arg::B}},
        {0x02, {Arg::D}},
        {0x03, {Arg::W}},
        {0x04, {}},
        {0x05, {}},
        {0x06, {Arg::D}},
        {0x07, {Arg::D}},
        {0x08, {}},
        {0x09, {}},
        {0x0a, {Arg::D}},
        {0x0b, {Arg::W}},
        {0x0c, {Arg::B}},
        {0x0e, {Arg::S}},
        {0x0f, {Arg::W}},
        {0x10, {Arg::B}},
        {0x11, {Arg::W}},
        {0x12, {Arg::B}},
        {0x14, {}},
        {0x15, {Arg::W}},
        {0x16, {Arg::B}},
        {0x17, {Arg::W}},
        {0x18, {Arg::B}},
        {0x19, {}},
        {0x1a, {}},
        {0x1b, {}},
        {0x1c, {}},
        {0x1d, {}},
        {0x1e, {}},
        {0x1f, {}},
        {0x20, {}},
        {0x21, {}},
        {0x22, {}},
        {0x23, {}},
        {0x24, {}},
        {0x25, {}},
        {0x26, {}},
        {0x27, {}},
    };
    return table;
}

std::uint32_t read_u32_le(const Bytes& d, std::size_t off) {
    return static_cast<std::uint32_t>(d[off]) |
           (static_cast<std::uint32_t>(d[off + 1]) << 8) |
           (static_cast<std::uint32_t>(d[off + 2]) << 16) |
           (static_cast<std::uint32_t>(d[off + 3]) << 24);
}

}  // namespace

const std::vector<Arg>* opcode_args(std::uint8_t op) {
    const auto& t = opcode_table();
    auto it = t.find(op);
    return it == t.end() ? nullptr : &it->second;
}

bool is_known_opcode(std::uint8_t op) { return opcode_args(op) != nullptr; }

std::vector<StringRef> extract_strings(const Bytes& data) {
    if (data.size() < 4) throw std::runtime_error("file too small to be a FVP .hcb");

    const std::uint32_t script_len = read_u32_le(data, 0);
    std::size_t pos = 4;
    const std::size_t end = std::min<std::size_t>(script_len, data.size());

    std::vector<StringRef> out;
    while (pos < end) {
        const std::size_t instr_off = pos;
        const std::uint8_t op = data[pos];
        ++pos;
        const std::vector<Arg>* args = opcode_args(op);
        if (!args) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "unknown opcode 0x%02x at 0x%08x",
                          op, static_cast<unsigned>(instr_off));
            throw std::runtime_error(buf);
        }
        for (Arg a : *args) {
            switch (a) {
            case Arg::B: pos += 1; break;
            case Arg::W: pos += 2; break;
            case Arg::D: pos += 4; break;
            case Arg::S: {
                // 0x0E <LEN_BYTE> <BYTES> 0x00 -- LEN_BYTE counts the NUL, so
                // the payload is LEN_BYTE - 1 bytes.  A truncated final record
                // would run the iterators past end(), so clamp both ends.
                const std::size_t str_len = data[pos];
                ++pos;
                const std::size_t take = str_len > 0 ? str_len - 1 : 0;
                const std::size_t lo = std::min(pos, data.size());
                const std::size_t hi = std::min(pos + take, data.size());
                StringRef ref;
                ref.offset = static_cast<std::uint32_t>(instr_off);
                ref.raw.assign(data.begin() + static_cast<std::ptrdiff_t>(lo),
                               data.begin() + static_cast<std::ptrdiff_t>(hi));
                pos += str_len;
                auto strict = cp932_to_utf8_strict(ref.raw);
                ref.decoded = strict ? *strict
                                     : cp932_to_utf8_replace(ref.raw.data(),
                                                             ref.raw.size());
                out.push_back(std::move(ref));
                break;
            }
            }
        }
    }
    return out;
}

std::vector<StringRef> load_all(const std::string& path) {
    return extract_strings(read_file(path));
}

std::vector<Event> walk_events(const Bytes& data) {
    std::vector<Event> events;
    if (data.size() < 4) return events;

    const std::uint32_t script_len = read_u32_le(data, 0);
    const std::size_t end = std::min<std::size_t>(script_len, data.size());

    std::size_t pos = 4;
    while (pos < end) {
        const std::uint8_t op = data[pos];
        const std::vector<Arg>* args = opcode_args(op);
        if (!args) break;
        ++pos;
        Event ev;
        ev.op = op;
        for (Arg a : *args) {
            switch (a) {
            case Arg::B: pos += 1; break;
            case Arg::W: pos += 2; break;
            case Arg::D: pos += 4; break;
            case Arg::S: {
                const std::size_t str_len = pos < data.size() ? data[pos] : 0;
                ++pos;
                const std::size_t take = str_len > 0 ? str_len - 1 : 0;
                const std::size_t lo = std::min(pos, data.size());
                const std::size_t hi = std::min(pos + take, data.size());
                ev.payload = cp932_to_utf8_strict(data.data() + lo, hi - lo);
                pos += str_len;
                break;
            }
            }
        }
        events.push_back(std::move(ev));
    }
    return events;
}

}  // namespace fav::fvp
