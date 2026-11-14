// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Keyless XOR-delta container ("ADIEUMEN" v1) serializer, parser, canonical
// MurmurHash2, relative-path security validator, and raw-asset-signature gate.
//
// Wire layout (little-endian, exact cross-architecture parity with Win32
// winmm.dll):
//   * 32-byte header:
//       +0x00: char magic[8] = {'A','D','I','E','U','M','E','N'}
//       +0x08: uint32_t version = 1
//       +0x0C: uint32_t record_count (1 .. 4096)
//       +0x10: uint32_t manifest_hash = MurmurHash2(bytes[0x20 .. EOF], 0)
//       +0x14: uint8_t reserved[12] = {0}
//   * Per-record stream starting at +0x20:
//       uint16_t source_path_len (1 .. 240), char source_rel_path[len]
//       uint16_t target_path_len (1 .. 240), char target_rel_path[len]
//       uint64_t orig_offset
//       uint64_t orig_size (64 .. 128 MiB)
//       uint32_t orig_slice_hash = MurmurHash2(orig_slice, 0)
//       uint64_t new_size (1 .. 128 MiB)
//       uint32_t new_raw_hash = MurmurHash2(new_raw, 0)
//       uint32_t flags (kFlagBackupStandalone = 0x01)
//       uint8_t xor_payload[new_size]
//
// Thread-compatible.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <boost/core/span.hpp>

namespace adi::menu {

inline constexpr std::array<char, 8> kMenuPatchMagic = {
    'A', 'D', 'I', 'E', 'U', 'M', 'E', 'N'};
inline constexpr std::uint32_t kMenuPatchVersion = 1u;
inline constexpr std::size_t kMenuPatchHeaderBytes = 32u;
inline constexpr std::size_t kMenuPatchReservedBytes = 12u;
inline constexpr std::uint32_t kMinMenuPatchRecords = 1u;
inline constexpr std::uint32_t kMaxMenuPatchRecords = 4096u;
inline constexpr std::size_t kMinMenuRelPathBytes = 1u;
inline constexpr std::size_t kMaxMenuRelPathBytes = 240u;
inline constexpr std::uint64_t kMinOrigSliceBytes = 64u;
inline constexpr std::uint64_t kMaxAssetPayloadBytes = 128ULL * 1024ULL * 1024ULL;
inline constexpr std::uint32_t kFlagLooseOverride = 0x00u;
inline constexpr std::uint32_t kFlagBackupStandalone = 0x01u;
inline constexpr std::size_t kForbiddenZeroRunBytes = 64u;

// Canonical 32-bit little-endian MurmurHash2 (`m = 0x5BD1E995u, r = 24, seed = 0`).
std::uint32_t MurmurHash2(const std::uint8_t* data, std::size_t len,
                          std::uint32_t seed = 0u) noexcept;
std::uint32_t MurmurHash2(boost::span<const std::uint8_t> data,
                          std::uint32_t seed = 0u) noexcept;

// Validates a stored relative asset path (`1..240` bytes, valid UTF-8, `'\\'`
// separator only; rejects leading/trailing slashes, `'/'`, drive colons `':'`,
// NUL or control characters, empty segments, `.` / `..` traversal segments,
// segments ending in `' '` or `'.'`, and DOS reserved device stems
// `CON`/`PRN`/`AUX`/`NUL`/`COM1..9`/`LPT1..9`).
bool IsSafeRelativeAssetPath(std::string_view rel_path) noexcept;

// Validates a stored target asset path (`IsSafeRelativeAssetPath` plus rejects
// leaf names `menu_patch.bin` / `menu_patch.applied` and forbidden executable
// or backup extensions `.exe`, `.dll`, `.bak`, `.tmp` case-insensitively).
bool IsAllowedMenuTargetPath(std::string_view target_rel_path) noexcept;

// Symmetric keyless XOR-delta mask:
//   mask[k] = orig_slice[k % orig_size]
//           ^ ((orig_slice_hash >> ((k & 3) * 8)) & 0xFF)
//           ^ ((k * 0x9E + 0x37) & 0xFF)
std::vector<std::uint8_t> ApplyMenuPatchXorMask(
    boost::span<const std::uint8_t> input,
    boost::span<const std::uint8_t> orig_slice,
    std::uint32_t orig_slice_hash);

struct MenuPatchRecordInput {
    std::string source_rel_path;
    std::string target_rel_path;
    std::uint64_t orig_offset = 0u;
    std::vector<std::uint8_t> orig_slice;
    std::vector<std::uint8_t> new_raw;
    std::uint32_t flags = kFlagBackupStandalone;
};

struct MenuPatchRecord {
    std::string source_rel_path;
    std::string target_rel_path;
    std::uint64_t orig_offset = 0u;
    std::uint64_t orig_size = 0u;
    std::uint32_t orig_slice_hash = 0u;
    std::uint64_t new_size = 0u;
    std::uint32_t new_raw_hash = 0u;
    std::uint32_t flags = kFlagBackupStandalone;
    std::vector<std::uint8_t> xor_payload;
};

struct MenuPatchManifest {
    std::uint32_t version = kMenuPatchVersion;
    std::uint32_t manifest_hash = 0u;
    std::vector<MenuPatchRecord> records;
};

// Parses and validates a complete "ADIEUMEN" v1 binary image.
// Throws std::runtime_error on any header, hash, bounds, or path violation.
MenuPatchManifest ParseMenuPatchContainer(boost::span<const std::uint8_t> blob);

// Verifies `orig_slice` against `record.orig_slice_hash`, unmasks
// `record.xor_payload`, and verifies `record.new_raw_hash`.
std::vector<std::uint8_t> UnmaskMenuPatchRecord(
    const MenuPatchRecord& record, boost::span<const std::uint8_t> orig_slice);

// Scans `blob[0x20 .. EOF)` and returns false (populating `*violation_out` when
// non-null) if PNG magic (`89 50 4E 47 0D 0A 1A 0A`), UTF-16LE `#GAMENAME` /
// `#DISCMARK`, or any run of `>= 64` consecutive zero bytes (`0x00`) is present.
bool VerifyNoRawAssetSignatures(boost::span<const std::uint8_t> blob,
                                std::string* violation_out = nullptr);

void AssertNoRawAssetSignatures(boost::span<const std::uint8_t> blob);

class MenuPatchBuilder {
public:
    MenuPatchBuilder();
    MenuPatchBuilder(const MenuPatchBuilder&) = delete;
    MenuPatchBuilder& operator=(const MenuPatchBuilder&) = delete;
    MenuPatchBuilder(MenuPatchBuilder&& other) noexcept;
    MenuPatchBuilder& operator=(MenuPatchBuilder&& other) noexcept;
    ~MenuPatchBuilder();

    void AddRecord(MenuPatchRecordInput input);
    std::size_t record_count() const noexcept;

    // Serializes all added records into an "ADIEUMEN" v1 container and enforces
    // `AssertNoRawAssetSignatures` before returning.
    std::vector<std::uint8_t> BuildBytes() const;

    void WriteToFile(const std::filesystem::path& output_path) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace adi::menu
