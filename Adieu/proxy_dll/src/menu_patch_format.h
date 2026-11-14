// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once

// OS-independent wire codec for Adieu's keyless XOR-delta menu container
// ("ADIEUMEN" v1) and fast-path applied sentinel ("ADIEUAPL" v1), plus
// canonical 32-bit MurmurHash2 and relative-path security validation.
//
// Free of Windows headers so unit and component tests execute the exact 32-bit
// object code linked into winmm.dll.
//
// "ADIEUMEN" v1 wire layout (little-endian):
//   Header (32 bytes):
//     +0x00: char     magic[8]        = "ADIEUMEN"
//     +0x08: uint32_t version         = 1
//     +0x0C: uint32_t record_count    in [1, 4096]
//     +0x10: uint32_t manifest_hash   = MurmurHash2(bytes[0x20 .. EOF], 0)
//     +0x14: uint8_t  reserved[12]    = {0}
//   Records (record_count entries starting at +0x20):
//     uint16_t source_path_len        in [1, 240]
//     char     source_rel_path[source_path_len]
//     uint16_t target_path_len        in [1, 240]
//     char     target_rel_path[target_path_len]
//     uint64_t orig_offset
//     uint64_t orig_size              in [64, 128 MiB]
//     uint32_t orig_slice_hash        = MurmurHash2(orig_slice, orig_size, 0)
//     uint64_t new_size               in [1, 128 MiB]
//     uint32_t new_raw_hash           = MurmurHash2(new_raw, new_size, 0)
//     uint32_t flags                  (kFlagBackupStandalone = 0x01)
//     uint8_t  xor_payload[new_size]
//
// "ADIEUAPL" v1 sentinel layout (little-endian):
//   +0x00: char     magic[8]          = "ADIEUAPL"
//   +0x08: uint32_t version           = 1
//   +0x0C: uint32_t manifest_hash
//   +0x10: uint64_t patch_file_size
//   +0x18: uint32_t entry_count       in [1, 4096]
//   Entries (entry_count items at +0x1C):
//     uint16_t path_len               in [1, 240]
//     char     rel_path[path_len]
//     uint64_t expected_new_size      in [1, 128 MiB]
//   Trailer (4 bytes):
//     uint32_t sentinel_hash          = MurmurHash2(bytes[0 .. end_of_entries], 0)

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace menu_patch_format {

// Non-owning view over a contiguous read-only byte range (C++17).
class ByteSpan {
public:
    constexpr ByteSpan() noexcept = default;
    constexpr ByteSpan(const std::uint8_t* data, std::size_t size) noexcept
        : data_(data), size_(size) {}

    template <std::size_t N>
    constexpr ByteSpan(const std::uint8_t (&arr)[N]) noexcept
        : data_(arr), size_(N) {}

    template <std::size_t N>
    constexpr ByteSpan(const std::array<std::uint8_t, N>& arr) noexcept
        : data_(arr.data()), size_(N) {}

    ByteSpan(const std::vector<std::uint8_t>& vec) noexcept
        : data_(vec.data()), size_(vec.size()) {}

    constexpr const std::uint8_t* data() const noexcept { return data_; }
    constexpr std::size_t size() const noexcept { return size_; }
    constexpr bool empty() const noexcept { return size_ == 0u; }
    constexpr const std::uint8_t* begin() const noexcept { return data_; }
    constexpr const std::uint8_t* end() const noexcept { return data_ + size_; }

    constexpr std::uint8_t operator[](std::size_t index) const noexcept {
        return data_[index];
    }

    // REQUIRES: offset <= size() && count <= size() - offset
    constexpr ByteSpan subspan(std::size_t offset,
                               std::size_t count) const noexcept {
        return ByteSpan(data_ + offset, count);
    }

    // REQUIRES: offset <= size()
    constexpr ByteSpan subspan(std::size_t offset) const noexcept {
        return ByteSpan(data_ + offset, size_ - offset);
    }

private:
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0u;
};

inline constexpr char kPatchMagic[8] = {
    'A', 'D', 'I', 'E', 'U', 'M', 'E', 'N',
};
inline constexpr char kSentinelMagic[8] = {
    'A', 'D', 'I', 'E', 'U', 'A', 'P', 'L',
};

inline constexpr std::size_t kMagicBytes = 8u;
inline constexpr std::uint32_t kFormatVersion = 1u;
inline constexpr std::size_t kPatchHeaderBytes = 32u;
inline constexpr std::size_t kPatchReservedBytes = 12u;
inline constexpr std::size_t kSentinelHeaderBytes = 28u;
inline constexpr std::size_t kSentinelTrailerHashBytes = 4u;

inline constexpr std::uint32_t kMinRecordCount = 1u;
inline constexpr std::uint32_t kMaxRecordCount = 4096u;
inline constexpr std::size_t kMinRelativePathBytes = 1u;
inline constexpr std::size_t kMaxRelativePathBytes = 240u;

inline constexpr std::uint64_t kMinOrigSliceBytes = 64u;
inline constexpr std::uint64_t kMaxAssetPayloadBytes =
    128ULL * 1024ULL * 1024ULL;

inline constexpr std::uint32_t kFlagBackupStandalone = 0x01u;
inline constexpr std::uint32_t kKnownFlagsMask = kFlagBackupStandalone;

inline constexpr std::uint32_t kMurmur2Multiplier = 0x5BD1E995u;
inline constexpr int kMurmur2Shift = 24;
inline constexpr std::uint32_t kDefaultSeed = 0u;
inline constexpr std::uint32_t kXorSaltStride = 0x9Eu;
inline constexpr std::uint32_t kXorSaltOffset = 0x37u;

// Canonical little-endian 32-bit MurmurHash2 (m = 0x5BD1E995, r = 24).
std::uint32_t MurmurHash2(const std::uint8_t* data,
                          std::size_t len,
                          std::uint32_t seed = kDefaultSeed) noexcept;
std::uint32_t MurmurHash2(ByteSpan bytes,
                          std::uint32_t seed = kDefaultSeed) noexcept;

// Computes the keyless XOR mask byte at payload position `index`:
//   orig_byte ^ ((orig_slice_hash >> ((index & 3) * 8)) & 0xFF)
//             ^ ((index * 0x9E + 0x37) & 0xFF)
std::uint8_t ComputeXorMaskByte(std::uint8_t orig_byte,
                                std::uint32_t orig_slice_hash,
                                std::size_t index) noexcept;

// Validates a relative path stored in "ADIEUMEN" / "ADIEUAPL" records:
//   - Byte length in [1, 240]
//   - Strictly valid UTF-8 (no overlongs, surrogates, or truncated sequences)
//   - Relative subpath with '\\' separators only: rejects leading/trailing
//     '\\', any '/', ':' drive designators, NUL/control bytes, Win32 reserved
//     punctuation (<>"|?*), empty segments, "." or ".." traversal segments,
//     segments ending in '.' or ' ', and Win32 device names (CON, NUL, etc.).
bool IsValidRelativePath(std::string_view rel_path) noexcept;

// Validates that `target_rel_path` passes IsValidRelativePath and does not
// target executable/DLL images, backup/temp suffixes (.bak, .tmp), or the
// patch container/sentinel filenames themselves.
bool IsAllowedTargetPath(std::string_view target_rel_path) noexcept;

// Pure arithmetic UTF-8 -> UTF-16 conversion without OS locale/NLS calls.
bool Utf8PathToWide(std::string_view utf8_path, std::wstring& wide_out);

enum class ManifestStatus {
    kOk,
    kTooSmall,
    kBadMagic,
    kUnsupportedVersion,
    kInvalidRecordCount,
    kNonZeroReserved,
    kManifestHashMismatch,
    kTruncatedRecord,
    kInvalidPath,
    kDuplicateTargetPath,
    kOrigSliceSizeOutOfRange,
    kPayloadSizeOutOfRange,
    kUnsupportedFlags,
    kTrailingBytes,
};

const char* ManifestStatusName(ManifestStatus status) noexcept;

struct MenuPatchHeader {
    std::uint32_t version = 0u;
    std::uint32_t record_count = 0u;
    std::uint32_t manifest_hash = 0u;
};

struct MenuPatchRecord {
    std::string source_rel_path;
    std::string target_rel_path;
    std::uint64_t orig_offset = 0u;
    std::uint64_t orig_size = 0u;
    std::uint32_t orig_slice_hash = 0u;
    std::uint64_t new_size = 0u;
    std::uint32_t new_raw_hash = 0u;
    std::uint32_t flags = 0u;
    std::size_t payload_offset = 0u;
};

struct MenuPatchManifest {
    MenuPatchHeader header;
    std::vector<MenuPatchRecord> records;
};

// Validates the 32-byte "ADIEUMEN" v1 header (magic, version, record_count,
// reserved == 0) without scanning record payloads.
ManifestStatus ParseMenuPatchHeader(ByteSpan header_bytes,
                                    MenuPatchHeader& out) noexcept;

// Parses and validates a complete "ADIEUMEN" v1 container blob, verifying
// header fields, manifest_hash over blob[0x20..EOF], all record paths/bounds,
// unique target paths, and exact consumption of `blob`.
ManifestStatus ParseMenuPatchManifest(ByteSpan blob, MenuPatchManifest& out);

struct MenuPatchRecordInput {
    std::string source_rel_path;
    std::string target_rel_path;
    std::uint64_t orig_offset = 0u;
    std::vector<std::uint8_t> orig_slice;
    std::vector<std::uint8_t> new_raw;
    std::uint32_t flags = kFlagBackupStandalone;
};

// Serializes validated records into an "ADIEUMEN" v1 container blob.
ManifestStatus SerializeMenuPatchManifest(
    const std::vector<MenuPatchRecordInput>& inputs,
    std::vector<std::uint8_t>& blob_out);

enum class UnmaskStatus {
    kOk,
    kOrigSliceSizeOutOfRange,
    kPayloadSizeOutOfRange,
    kOrigHashMismatch,
    kNewHashMismatch,
};

const char* UnmaskStatusName(UnmaskStatus status) noexcept;

// Verifies MurmurHash2(orig_slice) == expected_orig_hash, unmasks xor_payload,
// and verifies MurmurHash2(new_raw) == expected_new_hash before populating
// `new_raw_out`. Clears `new_raw_out` on any failure.
UnmaskStatus UnmaskRecordPayload(ByteSpan xor_payload,
                                 ByteSpan orig_slice,
                                 std::uint32_t expected_orig_hash,
                                 std::uint32_t expected_new_hash,
                                 std::vector<std::uint8_t>& new_raw_out);

enum class SentinelStatus {
    kOk,
    kTooSmall,
    kBadMagic,
    kUnsupportedVersion,
    kInvalidEntryCount,
    kTruncatedEntry,
    kInvalidPath,
    kInvalidExpectedSize,
    kTrailingBytes,
    kHashMismatch,
};

const char* SentinelStatusName(SentinelStatus status) noexcept;

struct AppliedSentinelEntry {
    std::string rel_path;
    std::uint64_t expected_new_size = 0u;
};

struct AppliedSentinel {
    std::uint32_t manifest_hash = 0u;
    std::uint64_t patch_file_size = 0u;
    std::vector<AppliedSentinelEntry> entries;
};

SentinelStatus SerializeAppliedSentinel(const AppliedSentinel& sentinel,
                                        std::vector<std::uint8_t>& bytes_out);

SentinelStatus ParseAppliedSentinel(ByteSpan blob, AppliedSentinel& out);

}  // namespace menu_patch_format
