// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "bgi_archive.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <stdexcept>

namespace snu::bgi {

namespace {

// asmodean PRNG: returns advance byte, updates state in place.
std::uint32_t hash_update(std::uint32_t& state) {
    std::uint32_t eax = (state & 0xFFFF) * 20021u;
    std::uint32_t edx = ((state >> 16) & 0xFFFF) * 20021u + state * 346u;
    edx = edx + (eax >> 16);
    state = (((edx & 0xFFFF) << 16) | (eax & 0xFFFF)) + 1u;
    return edx & 0x7FFF;
}

class BitReader {
public:
    BitReader(const Bytes& data, std::size_t pos) : data_(data), pos_(pos) {}

    int read_bit() {
        if (count_ == 0) {
            if (pos_ >= data_.size())
                throw std::runtime_error("DSC: bit stream exhausted");
            buf_ = data_[pos_++];
            count_ = 8;
        }
        int bit = (buf_ >> 7) & 1;
        buf_ = static_cast<std::uint8_t>(buf_ << 1);
        --count_;
        return bit;
    }

    std::uint32_t read_bits(int n) {
        std::uint32_t v = 0;
        for (int i = 0; i < n; ++i) v = (v << 1) | static_cast<std::uint32_t>(read_bit());
        return v;
    }

private:
    const Bytes& data_;
    std::size_t pos_;
    std::uint8_t buf_ = 0;
    int count_ = 0;
};

std::uint32_t rd32(const Bytes& d, std::size_t off) {
    std::uint32_t v;
    std::memcpy(&v, d.data() + off, 4);
    return v;  // little-endian host
}

void wr32(Bytes& d, std::uint32_t v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    d.insert(d.end(), p, p + 4);
}

constexpr char ARC_MAGIC[12] = {'B','U','R','I','K','O',' ','A','R','C','2','0'};

}  // namespace

Bytes dsc_decompress(const Bytes& data) {
    if (!is_dsc(data))
        throw std::runtime_error("not a DSC FORMAT 1.00 blob");

    std::uint32_t hash_key = rd32(data, 16);
    std::uint32_t dec_size = rd32(data, 20);
    // dword at 24 is dec_count, 28..32 reserved -- neither is needed to
    // decompress: dec_size alone bounds the output.

    // 512 code-length bytes, descrambled by PRNG.
    std::uint8_t code_lengths[512];
    std::size_t p = 32;
    for (int i = 0; i < 512; ++i) {
        std::uint32_t adv = hash_update(hash_key);
        code_lengths[i] = static_cast<std::uint8_t>(data[p + i] - adv);
    }
    p += 512;

    // Leaves sorted by (code length, symbol) -- canonical order.
    std::vector<std::pair<int, int>> leaves;  // (code_size, symbol)
    for (int sym = 0; sym < 512; ++sym)
        if (code_lengths[sym]) leaves.emplace_back(code_lengths[sym], sym);
    std::sort(leaves.begin(), leaves.end());

    if (leaves.empty()) return {};

    // Parallel-array tree: merge equal-max-depth leaf pairs until one root.
    struct Node { int left = 0, right = 0, symbol = 0; bool leaf = false; };
    std::vector<Node> nodes;
    nodes.reserve(1024);

    std::vector<std::pair<int, int>> work;  // (depth, node_id)
    for (auto [code_size, sym] : leaves) {
        Node n;
        n.leaf = true;
        n.symbol = sym;
        nodes.push_back(n);
        work.emplace_back(code_size, static_cast<int>(nodes.size()) - 1);
    }

    while (work.size() > 1) {
        int max_depth = 0;
        for (auto& [d, id] : work) max_depth = std::max(max_depth, d);
        std::vector<std::pair<int, int>> next;
        bool merged_any = false;
        std::size_t i = 0;
        while (i < work.size()) {
            if (i + 1 < work.size() && work[i].first == max_depth &&
                work[i + 1].first == max_depth) {
                Node parent;
                parent.left = work[i].second;
                parent.right = work[i + 1].second;
                nodes.push_back(parent);
                next.emplace_back(max_depth - 1, static_cast<int>(nodes.size()) - 1);
                i += 2;
                merged_any = true;
            } else {
                next.push_back(work[i]);
                ++i;
            }
        }
        work.swap(next);
        if (!merged_any)
            throw std::runtime_error("DSC: malformed Huffman code lengths");
    }
    if (work[0].first != 0)
        throw std::runtime_error("DSC: tree did not reduce to depth 0");
    const int root = work[0].second;

    // Decompress the bit stream.
    Bytes out(dec_size, 0);
    std::size_t out_pos = 0;
    BitReader br(data, p);

    while (out_pos < dec_size) {
        int node = root;
        while (!nodes[node].leaf)
            node = br.read_bit() ? nodes[node].right : nodes[node].left;
        int sym = nodes[node].symbol;
        if (sym < 256) {
            out[out_pos++] = static_cast<std::uint8_t>(sym);
        } else {
            std::uint32_t length = static_cast<std::uint32_t>(sym - 256) + 2;
            std::uint32_t offset = br.read_bits(12) + 2;
            if (offset > out_pos)
                throw std::runtime_error("DSC: LZ offset before buffer start");
            std::size_t src = out_pos - offset;
            for (std::uint32_t k = 0; k < length && out_pos < dec_size; ++k)
                out[out_pos++] = out[src++];
        }
    }
    return out;
}

std::vector<ArcEntry> list_arc(const std::string& path) {
    Bytes head_and_index;
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    char head[16];
    f.read(head, 16);
    if (!f || std::memcmp(head, ARC_MAGIC, 12) != 0)
        throw std::runtime_error(path + ": not a BURIKO ARC20 archive");
    std::uint32_t cnt;
    std::memcpy(&cnt, head + 12, 4);
    Bytes idx(static_cast<std::size_t>(cnt) * 128);
    f.read(reinterpret_cast<char*>(idx.data()), static_cast<std::streamsize>(idx.size()));
    if (!f) throw std::runtime_error(path + ": truncated ARC20 index");

    std::vector<ArcEntry> entries;
    entries.reserve(cnt);
    for (std::uint32_t i = 0; i < cnt; ++i) {
        const std::uint8_t* b = idx.data() + static_cast<std::size_t>(i) * 128;
        std::size_t name_len = 0;
        while (name_len < 96 && b[name_len] != 0) ++name_len;
        ArcEntry e;
        e.name = cp932_to_utf8_replace(b, name_len);
        std::memcpy(&e.offset, b + 96, 4);
        std::memcpy(&e.size, b + 100, 4);
        entries.push_back(std::move(e));
    }
    return entries;
}

namespace {
Bytes read_raw(const std::string& path, const std::vector<ArcEntry>& entries,
               std::size_t idx) {
    const auto& e = entries.at(idx);
    std::size_t data_start = 16 + entries.size() * 128;
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    f.seekg(static_cast<std::streamoff>(data_start + e.offset));
    Bytes raw(e.size);
    f.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
    if (!f) throw std::runtime_error(path + ": truncated entry payload");
    return raw;
}
}  // namespace

Bytes read_entry(const std::string& path, std::size_t idx, bool decompress) {
    auto entries = list_arc(path);
    Bytes raw = read_raw(path, entries, idx);
    if (decompress && is_dsc(raw)) return dsc_decompress(raw);
    return raw;
}

Bytes read_entry_by_name(const std::string& path, const std::string& name,
                         bool decompress) {
    auto entries = list_arc(path);
    for (std::size_t i = 0; i < entries.size(); ++i)
        if (entries[i].name == name) {
            Bytes raw = read_raw(path, entries, i);
            if (decompress && is_dsc(raw)) return dsc_decompress(raw);
            return raw;
        }
    throw std::out_of_range(name + " not in " + path);
}

std::size_t extract_all(const std::string& arc_path, const std::string& out_dir,
                        bool decompress) {
    std::filesystem::create_directories(std::filesystem::u8path(out_dir));
    auto entries = list_arc(arc_path);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        Bytes raw = read_raw(arc_path, entries, i);
        if (decompress && is_dsc(raw)) raw = dsc_decompress(raw);
        std::string safe = entries[i].name;
        for (auto& c : safe)
            if (c == '/' || c == '\\') c = '_';
        write_file(out_dir + "/" + safe, raw);
    }
    return entries.size();
}

void repack_arc(const std::string& in_path, const std::string& out_path,
                const std::map<std::string, Bytes>& replacements) {
    Bytes whole = read_file(in_path);
    if (whole.size() < 16 || std::memcmp(whole.data(), ARC_MAGIC, 12) != 0)
        throw std::runtime_error(in_path + ": not a BURIKO ARC20 archive");
    std::uint32_t cnt = rd32(whole, 12);
    std::size_t data_start = 16 + static_cast<std::size_t>(cnt) * 128;

    Bytes out_idx, out_data;
    out_idx.reserve(static_cast<std::size_t>(cnt) * 128);
    std::uint32_t cursor = 0;

    for (std::uint32_t i = 0; i < cnt; ++i) {
        const std::uint8_t* b = whole.data() + 16 + static_cast<std::size_t>(i) * 128;
        std::size_t name_len = 0;
        while (name_len < 96 && b[name_len] != 0) ++name_len;
        std::string name = cp932_to_utf8_replace(b, name_len);
        std::uint32_t off, sz;
        std::memcpy(&off, b + 96, 4);
        std::memcpy(&sz, b + 100, 4);

        const Bytes* payload;
        Bytes original;
        auto it = replacements.find(name);
        if (it != replacements.end()) {
            payload = &it->second;
        } else {
            original.assign(whole.begin() + static_cast<std::ptrdiff_t>(data_start + off),
                            whole.begin() + static_cast<std::ptrdiff_t>(data_start + off + sz));
            payload = &original;
        }

        out_idx.insert(out_idx.end(), b, b + 96);          // name field verbatim
        wr32(out_idx, cursor);
        wr32(out_idx, static_cast<std::uint32_t>(payload->size()));
        out_idx.insert(out_idx.end(), b + 104, b + 128);   // extra bytes preserved
        out_data.insert(out_data.end(), payload->begin(), payload->end());
        cursor += static_cast<std::uint32_t>(payload->size());
    }

    Bytes out;
    out.reserve(16 + out_idx.size() + out_data.size());
    out.insert(out.end(), ARC_MAGIC, ARC_MAGIC + 12);
    wr32(out, cnt);
    out.insert(out.end(), out_idx.begin(), out_idx.end());
    out.insert(out.end(), out_data.begin(), out_data.end());
    write_file(out_path, out);
}

}  // namespace snu::bgi
