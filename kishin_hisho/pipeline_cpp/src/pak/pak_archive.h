// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Nitroplus .pak v4 archive reader, writer, stream cipher, and CP932 codec for
// Kishin Hishou Demonbane (0script.pak, magic "KishinHisyouDB").
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/util.h"

namespace ksh::pak {

inline constexpr std::uint32_t kPakFormatVersion = 4;
inline constexpr std::string_view kDefaultPakMagicName = "KishinHisyouDB";

struct PakEntry {
    std::string relative_path;
    Bytes payload;
    bool compress = true;
};

// Nitroplus system.dll signed-byte multiplicative hash (multiplier 0x89).
std::uint32_t ComputePakKey(std::string_view text) noexcept;
std::uint32_t ComputePakKey(const std::uint8_t* data, std::size_t size) noexcept;

// 32-bit little-endian word stream XOR cipher used on .pak v4 entry payloads.
Bytes XorWordStream(const std::uint8_t* data, std::size_t size, std::uint32_t key);
Bytes XorWordStream(const Bytes& data, std::uint32_t key);

// Converts UTF-8 text into CP932 bytes with Demonbane symbol and NEC Kanji
// normalizations.
Bytes Utf8ToCp932(std::string_view utf8_text);

// Converts CP932 bytes into UTF-8 text.
std::string Cp932ToUtf8(const std::uint8_t* data, std::size_t size);
std::string Cp932ToUtf8(const Bytes& cp932_bytes);

// Thread-compatible builder for Nitroplus .pak v4 archives.
class PakArchiveWriter {
public:
    explicit PakArchiveWriter(std::string magic_name = std::string(kDefaultPakMagicName));
    PakArchiveWriter(const PakArchiveWriter& other);
    PakArchiveWriter(PakArchiveWriter&& other) noexcept;
    PakArchiveWriter& operator=(const PakArchiveWriter& other);
    PakArchiveWriter& operator=(PakArchiveWriter&& other) noexcept;
    ~PakArchiveWriter();

    void AddEntry(std::string relative_path, Bytes payload, bool compress = true);
    void AddEntry(PakEntry entry);
    void Clear();

    std::size_t EntryCount() const noexcept;
    const std::vector<PakEntry>& Entries() const noexcept;

    Bytes BuildToBytes() const;
    void WriteToFile(const std::string& output_path) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Thread-compatible reader and verifier for Nitroplus .pak v4 archives.
class PakArchiveReader {
public:
    explicit PakArchiveReader(const Bytes& archive_bytes);
    PakArchiveReader(const PakArchiveReader& other);
    PakArchiveReader(PakArchiveReader&& other) noexcept;
    PakArchiveReader& operator=(const PakArchiveReader& other);
    PakArchiveReader& operator=(PakArchiveReader&& other) noexcept;
    ~PakArchiveReader();

    static PakArchiveReader FromFile(const std::string& archive_path);

    std::uint32_t Version() const noexcept;
    const std::string& MagicName() const noexcept;
    const std::vector<PakEntry>& Entries() const noexcept;

    const PakEntry* FindEntry(std::string_view relative_path) const noexcept;
    void VerifyAgainst(const std::vector<PakEntry>& expected_entries) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

Bytes WritePakArchive(const std::vector<PakEntry>& entries,
                      std::string_view magic_name = kDefaultPakMagicName);
std::vector<PakEntry> ReadPakArchive(const Bytes& archive_bytes);
void VerifyPakArchive(const Bytes& archive_bytes,
                      const std::vector<PakEntry>& expected_entries);

}  // namespace ksh::pak
