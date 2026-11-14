// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Keyless XOR-delta container ("AMK2MENU" v1) for menu and UI overrides.
//
// Every modified asset is stored XOR-masked against a byte slice (>= 64 bytes)
// of the user's unmodified retail archive or standalone file, keyed with the
// slice's MurmurHash2 digest and a linear byte stream.  The serialized bundle
// contains zero raw HG-3, PNG, JPEG, FES, or KCS headers.
//
// Thread-compatible.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ama::menu {

inline constexpr char kMenuPatchMagic[8] = {'A', 'M', 'K', '2', 'M', 'E', 'N', 'U'};
inline constexpr std::uint32_t kMenuPatchFormatVersion = 1;
inline constexpr std::size_t kMenuPatchHeaderSize = 32;
inline constexpr std::size_t kMenuPatchHeaderReservedBytes = 12;
inline constexpr std::size_t kMenuPatchRecordReservedFlagBytes = 3;
inline constexpr std::uint32_t kMinOrigSliceBytes = 64;

inline constexpr std::uint8_t kFlagLooseOverride = 0x00;
inline constexpr std::uint8_t kFlagBackupStandalone = 0x01;

// 32-bit MurmurHash2 (m = 0x5BD1E995, r = 24).
std::uint32_t MurmurHash2(const std::uint8_t* data, std::size_t len,
                          std::uint32_t seed = 0);

inline std::uint32_t MurmurHash2(const std::vector<std::uint8_t>& data,
                                  std::uint32_t seed = 0) {
    return MurmurHash2(data.data(), data.size(), seed);
}

struct MenuPatchRecordInput {
    std::string source_rel_path;
    std::string target_rel_path;
    std::uint8_t flags = kFlagLooseOverride;
    std::uint64_t orig_offset = 0;
    std::vector<std::uint8_t> orig_slice;
    std::vector<std::uint8_t> new_raw;
};

struct MenuPatchRecord {
    std::string source_rel_path;
    std::string target_rel_path;
    std::uint8_t flags = kFlagLooseOverride;
    std::uint64_t orig_offset = 0;
    std::uint32_t orig_size = 0;
    std::uint32_t orig_slice_hash = 0;
    std::uint32_t new_raw_size = 0;
    std::uint32_t new_raw_hash = 0;
    std::vector<std::uint8_t> xor_payload;
};

struct MenuPatchContainerData {
    std::uint32_t format_version = kMenuPatchFormatVersion;
    std::uint32_t manifest_hash = 0;
    std::vector<MenuPatchRecord> records;
};

// Computes the manifest MurmurHash2 over concatenated per-record descriptors:
// [u16 src_len, src_bytes, u16 dst_len, dst_bytes, u8 flags, u64 orig_offset,
//  u32 orig_size, u32 orig_slice_hash, u32 new_raw_size, u32 new_raw_hash].
std::uint32_t ComputeManifestHash(const std::vector<MenuPatchRecord>& records);

// Applies the symmetric per-byte XOR mask between `input` and `orig_slice`.
std::vector<std::uint8_t> ApplyMenuPatchXorMask(
    const std::uint8_t* input, std::size_t input_size,
    const std::uint8_t* orig_slice, std::size_t orig_size,
    std::uint32_t orig_slice_hash);

// Verifies `orig_slice` against `record.orig_slice_hash`, unmasks
// `record.xor_payload`, and verifies `record.new_raw_hash`.
std::vector<std::uint8_t> UnmaskMenuPatchRecord(
    const MenuPatchRecord& record, const std::vector<std::uint8_t>& orig_slice);

// Returns true iff `data` contains zero occurrences of raw asset signatures
// ("HG-3", "\x89PNG", "JFIF", "FES\0", "KCS\0").
bool VerifyNoRawAssetSignatures(const std::uint8_t* data, std::size_t len);

inline bool VerifyNoRawAssetSignatures(const std::vector<std::uint8_t>& data) {
    return VerifyNoRawAssetSignatures(data.data(), data.size());
}

// Parses and validates a serialized "AMK2MENU" v1 container buffer.
MenuPatchContainerData ParseMenuPatchContainer(const std::uint8_t* data,
                                               std::size_t len);

inline MenuPatchContainerData ParseMenuPatchContainer(
    const std::vector<std::uint8_t>& data) {
    return ParseMenuPatchContainer(data.data(), data.size());
}

class MenuPatchBuilder {
public:
    MenuPatchBuilder();
    MenuPatchBuilder(const MenuPatchBuilder&) = delete;
    MenuPatchBuilder& operator=(const MenuPatchBuilder&) = delete;
    MenuPatchBuilder(MenuPatchBuilder&&) noexcept;
    MenuPatchBuilder& operator=(MenuPatchBuilder&&) noexcept;
    ~MenuPatchBuilder();

    void AddRecord(MenuPatchRecordInput input);
    std::size_t record_count() const;

    // Serializes all added records into an "AMK2MENU" v1 binary image and
    // asserts VerifyNoRawAssetSignatures on the output.
    std::vector<std::uint8_t> BuildBytes() const;

    void WriteToFile(const std::filesystem::path& output_path) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ama::menu
