// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once
// menu_patch_format.h  (amakano2pe / CatSystem2)
//
// Pure OS-independent reader for the keyless "AMK2MENU" v1 XOR-delta
// container shipped as <GameDir>\menu_patch.bin, plus the codec for the
// <GameDir>\menu_patch.applied sentinel that lets later launches skip work.
//
// Wire layout (all integers little-endian):
//   File header (32 bytes):
//     char     magic[8] = "AMK2MENU"
//     u32      format_version = 1
//     u32      record_count
//     u32      manifest_hash   MurmurHash2 over every record descriptor
//     u8       reserved[12] = 0
//   Record (repeated record_count times):
//     u16      source_path_len, char source_rel_path[source_path_len]
//     u16      target_path_len, char target_rel_path[target_path_len]
//     u8       flags            kFlagBackupStandalone or 0
//     u8       reserved_flags[3] = 0
//     u64      orig_offset
//     u32      orig_size        >= kMinOrigSliceSize
//     u32      orig_slice_hash  MurmurHash2(orig_slice)
//     u32      new_raw_size
//     u32      new_raw_hash     MurmurHash2(new_raw)
//     u8       xor_payload[new_raw_size]
//   A record descriptor is the record without reserved_flags and payload:
//     [u16 src_len][src][u16 dst_len][dst][u8 flags][u64 orig_offset]
//     [u32 orig_size][u32 orig_slice_hash][u32 new_raw_size][u32 new_raw_hash]
//   xor_payload[k] = new_raw[k] ^ orig_slice[k % orig_size]
//                  ^ byte(orig_slice_hash >> ((k & 3) * 8))
//                  ^ byte(k * 0x9E + 0x37)
// Paths are UTF-8, relative to <GameDir>, with `\` separators.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace menu_patch {

// Non-owning view over a contiguous byte range.
struct ByteView {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

inline constexpr std::size_t kMagicSize = 8;
inline constexpr std::uint32_t kFormatVersion = 1;
inline constexpr std::size_t kFileHeaderSize = 32;
inline constexpr std::uint32_t kMinOrigSliceSize = 64;
inline constexpr std::uint8_t kFlagBackupStandalone = 0x01;
inline constexpr std::uint8_t kKnownFlagsMask = kFlagBackupStandalone;

// MurmurHash2 (m = 0x5BD1E995, r = 24) with seed 0.
std::uint32_t MurmurHash2(const std::uint8_t* data, std::size_t len) noexcept;
std::uint32_t MurmurHash2Seeded(const std::uint8_t* data,
                                std::size_t len,
                                std::uint32_t seed) noexcept;

// Key-stream byte that masks payload byte `index` of a record.
std::uint8_t XorMaskByte(ByteView orig_slice,
                         std::uint32_t orig_slice_hash,
                         std::size_t index) noexcept;

enum class ParseStatus {
    kOk,
    kTooSmall,
    kBadMagic,
    kBadVersion,
    kReservedNotZero,
    kTruncated,
    kUnknownFlags,
    kOrigSliceTooSmall,
    kEmptyPayload,
    kUnsafePath,
    kFlagPathMismatch,
    kForbiddenTarget,
    kDuplicateTarget,
    kTrailingBytes,
    kManifestHashMismatch,
};

const char* ParseStatusName(ParseStatus status) noexcept;

struct MenuPatchHeader {
    std::uint32_t format_version = 0;
    std::uint32_t record_count = 0;
    std::uint32_t manifest_hash = 0;
};

struct MenuPatchRecord {
    std::string source_rel_path;
    std::string target_rel_path;
    std::uint8_t flags = 0;
    std::uint64_t orig_offset = 0;
    std::uint32_t orig_size = 0;
    std::uint32_t orig_slice_hash = 0;
    std::uint32_t new_raw_size = 0;
    std::uint32_t new_raw_hash = 0;
    std::size_t payload_offset = 0;  // into the container bytes
};

struct MenuPatchManifest {
    MenuPatchHeader header;
    std::vector<MenuPatchRecord> records;
};

bool IsBackupStandalone(const MenuPatchRecord& record) noexcept;

// Validates the 32-byte file header only (magic, version, reserved bytes).
ParseStatus ParseMenuPatchHeader(ByteView container, MenuPatchHeader& out);

// Validates the whole container: header, every record's bounds, flags,
// reserved bytes and paths, the manifest hash, and the absence of trailing
// bytes. On success `out` lists every record with its payload offset.
ParseStatus ParseMenuPatchContainer(ByteView container, MenuPatchManifest& out);

// True iff `utf8_path` is a non-empty, strictly valid UTF-8 relative path
// made of `\`-separated components, none of which is empty, `.`, `..`, or
// ends in `.` or space, and which contains no drive colon, `/`, control
// character, or Win32-reserved character (`<>"|?*`).
bool IsSafeRelativePath(std::string_view utf8_path) noexcept;

// True iff `utf8_path` names a file the auto-patcher must never write: a
// retail archive or executable image (`.int`, `.exe`, `.dll`), a backup or
// scratch file (`.bak`, `.tmp`), or the patch bundle and its sentinel.
bool IsForbiddenTarget(std::string_view utf8_path) noexcept;

// ASCII case-insensitive equality (Win32 paths compare case-insensitively).
bool PathsEqualIgnoreAsciiCase(std::string_view lhs,
                               std::string_view rhs) noexcept;

enum class UnmaskStatus {
    kOk,
    kPayloadOutOfRange,
    kOrigSliceSizeMismatch,
    kOrigSliceHashMismatch,
    kNewRawHashMismatch,
};

const char* UnmaskStatusName(UnmaskStatus status) noexcept;

// Reconstructs a record's new raw bytes from its masked payload inside
// `container` and the original slice read from disk, verifying both the
// original-slice digest and the reconstructed digest.
UnmaskStatus UnmaskRecordPayload(ByteView container,
                                 const MenuPatchRecord& record,
                                 ByteView orig_slice,
                                 std::vector<std::uint8_t>& new_raw_out);

// One reconstructed override recorded by the applied sentinel.
struct AppliedTarget {
    std::string target_rel_path;
    std::uint64_t size = 0;
};

// Contents of <GameDir>\menu_patch.applied: which bundle was applied and
// which files it produced.
struct AppliedSentinel {
    std::uint32_t manifest_hash = 0;
    std::uint64_t patch_file_size = 0;
    std::vector<AppliedTarget> targets;
};

std::vector<std::uint8_t> SerializeAppliedSentinel(
    const AppliedSentinel& sentinel);

// Returns false on any truncation, bad magic/version, unsafe path, trailing
// bytes, or integrity-digest mismatch.
bool ParseAppliedSentinel(ByteView bytes, AppliedSentinel& out);

}  // namespace menu_patch
