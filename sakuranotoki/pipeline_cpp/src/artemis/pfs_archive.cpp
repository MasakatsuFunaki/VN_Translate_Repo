// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "pfs_archive.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <utility>

#include <openssl/sha.h>

namespace snt::pfs {

namespace fs = std::filesystem;

namespace {

// The archive suffixes in the order build_overlay reads them.  A name held by
// more than one archive is taken from the last archive in this list that holds
// it.
//
// The .010 update supersedes the base by content: it declares game_ver 1.0.1
// where the base declares 1.0.0 and carries four proofreading fixes.  No other
// archive holds a script.  The general higher-suffix ordering is consistent
// with this corpus but is not proven by it, which is why every copy that loses
// is named on the log: an archive that contradicts the ordering then shows up
// as a line a reader can check, instead of being chosen in silence.
const std::vector<std::string> ARCHIVE_ORDER = {".pfs", ".pfs.000", ".pfs.001",
                                                ".pfs.002", ".pfs.010"};

std::uint32_t rd_u32(const Bytes& b, std::size_t off) {
    if (off + 4 > b.size()) throw std::runtime_error("PF8 index truncated");
    return static_cast<std::uint32_t>(b[off]) |
           (static_cast<std::uint32_t>(b[off + 1]) << 8) |
           (static_cast<std::uint32_t>(b[off + 2]) << 16) |
           (static_cast<std::uint32_t>(b[off + 3]) << 24);
}

}  // namespace

Bytes read_index(const std::string& path, std::uint64_t* file_total) {
    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::uint8_t head[7];
    f.read(reinterpret_cast<char*>(head), 7);
    if (!f || std::memcmp(head, "pf8", 3) != 0)
        throw std::runtime_error(path + ": not a PF8 archive");
    const std::uint32_t index_size =
        static_cast<std::uint32_t>(head[3]) | (static_cast<std::uint32_t>(head[4]) << 8) |
        (static_cast<std::uint32_t>(head[5]) << 16) |
        (static_cast<std::uint32_t>(head[6]) << 24);
    Bytes index(index_size);
    f.read(reinterpret_cast<char*>(index.data()), index_size);
    if (static_cast<std::uint32_t>(f.gcount()) != index_size)
        throw std::runtime_error(path + ": short index read");
    if (file_total) {
        f.clear();
        f.seekg(0, std::ios::end);
        *file_total = static_cast<std::uint64_t>(f.tellg());
    }
    return index;
}

Key archive_key(const std::string& path) {
    Bytes index = read_index(path);
    Key key{};
    SHA1(index.data(), index.size(), key.data());
    return key;
}

std::vector<Entry> list_pfs(const std::string& path, std::uint64_t* file_total) {
    Bytes index = read_index(path, file_total);
    std::vector<Entry> entries;
    std::size_t p = 0;
    const std::uint32_t entry_count = rd_u32(index, p);
    p += 4;
    entries.reserve(entry_count);
    for (std::uint32_t i = 0; i < entry_count; ++i) {
        const std::uint32_t name_len = rd_u32(index, p);
        p += 4;
        if (p + name_len > index.size()) throw std::runtime_error("PF8 name overruns index");
        auto strict = cp932_to_utf8_strict(index.data() + p, name_len);
        Entry e;
        e.name = strict ? *strict : cp932_to_utf8_replace(index.data() + p, name_len);
        p += name_len;
        // 4 bytes reserved, 4 bytes offset, 4 bytes size
        (void)rd_u32(index, p);
        e.offset = rd_u32(index, p + 4);
        e.size = rd_u32(index, p + 8);
        p += 12;
        entries.push_back(std::move(e));
    }
    return entries;
}

void xor_decrypt(Bytes& data, const Key& key, std::size_t start_offset) {
    for (std::size_t i = 0; i < data.size(); ++i)
        data[i] ^= key[(start_offset + i) % KEY_LEN];
}

Bytes read_entry(const std::string& path, std::size_t idx, const Key& key) {
    std::uint64_t file_total = 0;
    std::vector<Entry> entries = list_pfs(path, &file_total);
    if (idx >= entries.size()) throw std::runtime_error("PF8 entry index out of range");
    const Entry& e = entries[idx];
    const std::uint64_t end = std::min<std::uint64_t>(
        static_cast<std::uint64_t>(e.offset) + e.size, file_total);
    const std::size_t want =
        end > e.offset ? static_cast<std::size_t>(end - e.offset) : 0;

    std::ifstream f(fs::u8path(path), std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    f.seekg(e.offset);
    Bytes raw(want);
    if (want) f.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(want));
    raw.resize(static_cast<std::size_t>(f.gcount()));
    // Each file restarts the key cycle.
    xor_decrypt(raw, key, 0);
    return raw;
}

Overlay build_overlay(const std::string& game_dir, const std::string& base_name) {
    Overlay ov;
    for (const auto& suffix : ARCHIVE_ORDER) {
        const std::string fname = base_name + suffix;
        const std::string path = game_dir + "\\" + fname;
        if (!fs::is_regular_file(fs::u8path(path))) continue;
        std::vector<Entry> entries;
        try {
            entries = list_pfs(path);
        } catch (const std::exception& e) {
            print_line("  [skip] " + fname + ": " + e.what());
            continue;
        }
        ov.archives.push_back(path);
        ov.keys[path] = archive_key(path);
        for (std::size_t i = 0; i < entries.size(); ++i) {
            auto [it, inserted] =
                ov.entries.emplace(entries[i].name, std::make_pair(path, i));
            if (!inserted) {
                print_line("  [shadowed] " + entries[i].name + " in " +
                           fs::u8path(it->second.first).filename().u8string() +
                           " loses to " + fname);
                it->second = {path, i};
            }
        }
    }
    return ov;
}

}  // namespace snt::pfs
