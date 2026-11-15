// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// menu_patch_format.h
//
// OS-independent C++17 codec for CROSS_CHANNEL's keyless XOR-delta menu
// archive patch (menu_patch.bin -> data\sys.cpk).
//
// Free of Windows headers so the exact same object code links into both
// xinput1_3.dll and the unit-test binaries.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace menu_patch {

inline constexpr std::size_t kPatchMagicSize = 4u;
inline constexpr char kPatchMagic[kPatchMagicSize] = {'C', 'C', 'M', 'P'};
inline constexpr std::uint32_t kPatchFormatVersion = 1u;
inline constexpr std::size_t kPatchHeaderSize = 32u;

inline constexpr std::size_t kLzssRingSize = 4096u;
inline constexpr std::size_t kLzssInitialRingPos = 0xFEEu;
inline constexpr std::size_t kLzssMinMatch = 3u;
inline constexpr std::size_t kLzssMaxMatch = 18u;

inline constexpr std::uint32_t kCpkDefaultAlign = 0x800u;

struct PatchHeaderInfo {
    std::uint32_t version = 0;
    std::uint32_t record_count = 0;
    std::uint32_t patch_payload_hash = 0;
    std::uint64_t expected_orig_cpk_size = 0;
    std::uint64_t expected_patched_cpk_size = 0;
};

struct PakEntrySlice {
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
};

struct CpkEntrySlice {
    std::uint16_t id = 0;
    bool is_low_table = false;
    std::uint64_t offset = 0;
    std::uint32_t size = 0;
};

struct SubTextureReplacement {
    std::uint32_t cpk_entry_id = 0;
    std::uint32_t pak_sub_index = 0;
    std::vector<std::uint8_t> orig_bytes;
    std::vector<std::uint8_t> new_bytes;
};

// MurmurHash2 (32-bit, seed = size, m = 0x5BD1E995, r = 24).
std::uint32_t ComputeMurmurHash2(const std::uint8_t* data,
                                 std::size_t size) noexcept;
std::uint32_t ComputeMurmurHash2(const std::vector<std::uint8_t>& data) noexcept;

// LZSS codec matching cc.exe RVA 0x45910:
//   - 4-byte LE uncompressed size prefix followed by flag/token stream
//   - 4096-byte ring buffer initialized to 0x00, starting write cursor 0xFEE
//   - flag bit 1 = literal byte; flag bit 0 = 12-bit ring offset + 4-bit (len-3)
bool LzssDecompressZeroRing(const std::uint8_t* src,
                            std::size_t src_size,
                            std::vector<std::uint8_t>& out);
bool LzssDecompressZeroRing(const std::vector<std::uint8_t>& src,
                            std::vector<std::uint8_t>& out);

bool LzssCompressZeroRing(const std::uint8_t* src,
                          std::size_t src_size,
                          std::vector<std::uint8_t>& out);
bool LzssCompressZeroRing(const std::vector<std::uint8_t>& src,
                          std::vector<std::uint8_t>& out);

// Inner .pak container directory parser and rebuilder.
// Format: N * 16-byte records [LE32 offset, LE32 size, 0, 0] where
// N = first_offset / 16, followed by tightly-packed sub-entry payloads.
bool ParsePakArchive(const std::vector<std::uint8_t>& pak_bytes,
                     std::vector<PakEntrySlice>& out_entries);

bool RebuildPakArchive(
    const std::vector<std::uint8_t>& orig_pak,
    const std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>>&
        replacements,
    std::vector<std::uint8_t>& out_pak);

// Outer Criware CPK (ITOC-indexed, unencrypted @UTF CpkHeader + CpkItocInfo)
// parser and rebuilder for data\sys.cpk.
bool ParseSysCpkToc(const std::vector<std::uint8_t>& cpk_bytes,
                    std::vector<CpkEntrySlice>& out_entries);

bool RebuildSysCpk(
    const std::vector<std::uint8_t>& orig_cpk,
    const std::vector<std::pair<std::uint32_t, std::vector<std::uint8_t>>>&
        entry_replacements,
    std::vector<std::uint8_t>& out_patched_cpk);

// Stateful CCMP v1 patch package (PIMPL-insulated).
class MenuPatchPackage {
public:
    MenuPatchPackage();
    ~MenuPatchPackage();

    MenuPatchPackage(MenuPatchPackage&&) noexcept;
    MenuPatchPackage& operator=(MenuPatchPackage&&) noexcept;

    MenuPatchPackage(const MenuPatchPackage&) = delete;
    MenuPatchPackage& operator=(const MenuPatchPackage&) = delete;

    // Fast-path 32-byte header parser (validates magic + version without
    // reading or hashing the record payload).
    static bool ParseHeaderPrefix(const std::uint8_t* data,
                                  std::size_t size,
                                  PatchHeaderInfo& out_info) noexcept;

    // Serializes a keyless XOR-delta CCMP v1 patch blob against `orig_cpk` and
    // validates full round-trip reconstruction. Optionally outputs the rebuilt
    // CPK image in `out_patched_cpk`.
    static bool BuildPatchBlob(
        const std::vector<std::uint8_t>& orig_cpk,
        const std::vector<SubTextureReplacement>& replacements,
        std::vector<std::uint8_t>& out_patch_blob,
        std::vector<std::uint8_t>* out_patched_cpk = nullptr);

    bool LoadFromMemory(const std::uint8_t* data, std::size_t size);
    bool LoadFromMemory(const std::vector<std::uint8_t>& patch_blob);

    bool IsLoaded() const noexcept;
    const PatchHeaderInfo& Header() const noexcept;
    std::size_t RecordCount() const noexcept;

    bool ApplyToSysCpk(const std::vector<std::uint8_t>& orig_cpk,
                       std::vector<std::uint8_t>& out_patched_cpk) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Convenience single-call in-memory applier.
bool ApplyMenuPatchInMemory(const std::vector<std::uint8_t>& orig_cpk,
                            const std::vector<std::uint8_t>& patch_blob,
                            std::vector<std::uint8_t>& out_patched_cpk);

}  // namespace menu_patch
