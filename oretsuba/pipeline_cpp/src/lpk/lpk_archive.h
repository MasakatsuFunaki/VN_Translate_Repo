// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/util.h"

namespace oretsuba::lpk {

constexpr std::uint32_t kInitKey0 = 0xA5B9AC6BU;
constexpr std::uint32_t kInitKey1 = 0x9A639DE5U;
constexpr std::uint32_t kIndexCipherSeed = 0x31746285U;
constexpr std::size_t kRingSize = 0x1000U;
constexpr std::size_t kInitialRingPos = 0x0FEEU;

struct LpkKeyPair {
    std::uint32_t file_key = 0;
    std::uint32_t header_key = 0;
};

struct LpkEntryInfo {
    std::uint32_t index = 0;
    std::string name;
    std::uint8_t entry_flags = 0;
    std::uint32_t offset = 0;
    std::uint32_t packed_size = 0;
    std::uint32_t unpacked_size = 0;
};

LpkKeyPair DeriveLpkKeys(std::string_view stem,
                         std::uint32_t master_key0 = 0,
                         std::uint32_t master_key1 = 0);

Bytes DecompressLzss(const std::uint8_t* src,
                     std::size_t src_size,
                     std::size_t unpacked_size);

Bytes DecompressLzss(const Bytes& src, std::size_t unpacked_size);

// Thread-compatible reader for Lucifen LPK1 archives.
class LpkArchiveReader {
public:
    explicit LpkArchiveReader(const std::string& archive_path,
                              std::uint32_t master_key0 = 0,
                              std::uint32_t master_key1 = 0);
    LpkArchiveReader(Bytes archive_bytes,
                     std::string_view stem,
                     std::uint32_t master_key0 = 0,
                     std::uint32_t master_key1 = 0);

    LpkArchiveReader(const LpkArchiveReader&) = delete;
    LpkArchiveReader& operator=(const LpkArchiveReader&) = delete;
    LpkArchiveReader(LpkArchiveReader&& other) noexcept;
    LpkArchiveReader& operator=(LpkArchiveReader&& other) noexcept;
    ~LpkArchiveReader();

    std::size_t EntryCount() const;
    const std::vector<LpkEntryInfo>& Entries() const;
    std::optional<std::size_t> FindEntryIndex(std::string_view name) const;
    Bytes ReadEntry(std::size_t entry_index) const;
    Bytes ReadEntryByName(std::string_view name) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace oretsuba::lpk
