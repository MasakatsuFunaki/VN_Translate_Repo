// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu_autopatch.h"

#include "log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace menu_autopatch {

namespace {

using Bytes = std::vector<std::uint8_t>;

constexpr wchar_t kPatchMutexName[] = L"Local\\MushiguruiMenuPatchMutex";
constexpr wchar_t kPatchBinFileName[] = L"menu_patch.bin";
constexpr wchar_t kGpkExtension[] = L".gpk";
constexpr wchar_t kGtbExtension[] = L".gtb";
constexpr wchar_t kBackupSuffix[] = L".bak";
constexpr wchar_t kTempSuffix[] = L".tmp";

constexpr std::size_t kPatchMagicSize = 8u;
constexpr char kPatchMagic[kPatchMagicSize] = {'M', 'S', 'G', 'M', 'E', 'N', 'U', '1'};
constexpr std::size_t kPatchHeaderSize = 32u;
constexpr std::uint32_t kPatchFormatVersion = 1u;

constexpr std::size_t kHeaderOffsetVersion = 0x08u;
constexpr std::size_t kHeaderOffsetStandaloneCount = 0x0Cu;
constexpr std::size_t kHeaderOffsetArchiveCount = 0x10u;
constexpr std::size_t kHeaderOffsetFlags = 0x14u;
constexpr std::size_t kHeaderOffsetReserved = 0x18u;

constexpr std::size_t kU16FieldBytes = 2u;
constexpr std::size_t kStandaloneVariantHeaderBytes = 16u;
constexpr std::size_t kVariantOrigSizeOffset = 0u;
constexpr std::size_t kVariantOrigHashOffset = 4u;
constexpr std::size_t kVariantNewSizeOffset = 8u;
constexpr std::size_t kVariantNewHashOffset = 12u;

constexpr std::size_t kArchiveFixedHeaderBytes = 36u;
constexpr std::size_t kArchiveOrigGpkSizeOffset = 0u;
constexpr std::size_t kArchiveNewGpkSizeOffset = 8u;
constexpr std::size_t kArchiveOrigGtbSizeOffset = 16u;
constexpr std::size_t kArchiveOrigGtbHashOffset = 20u;
constexpr std::size_t kArchiveNewGtbSizeOffset = 24u;
constexpr std::size_t kArchiveNewGtbHashOffset = 28u;
constexpr std::size_t kArchiveEntryCountOffset = 32u;

constexpr std::size_t kArchiveEntryHeaderBytes = 28u;
constexpr std::size_t kEntryIndexOffset = 0u;
constexpr std::size_t kEntryOrigOffsetOffset = 4u;
constexpr std::size_t kEntryOrigSizeOffset = 12u;
constexpr std::size_t kEntryOrigHashOffset = 16u;
constexpr std::size_t kEntryNewSizeOffset = 20u;
constexpr std::size_t kEntryNewHashOffset = 24u;

constexpr std::size_t kGtbCountFieldBytes = 4u;
constexpr std::size_t kGtbU32EntryStride = 4u;
constexpr std::size_t kGtbU32TableCount = 2u;
constexpr std::size_t kGtbU64EntryStride = 8u;
constexpr std::size_t kGtbOver2GTrailerSize = 8u;
constexpr std::size_t kGtbOver2GFooterPaddingBytes = 16u;
constexpr char kGtbOver2GTrailer[kGtbOver2GTrailerSize] = {
    'o', 'v', 'e', 'r', '2', 'G', '!', '\0'};

constexpr std::uint32_t kMaxStandaloneGroups = 256u;
constexpr std::uint16_t kMaxVariantsPerGroup = 16u;
constexpr std::uint32_t kMaxArchiveSections = 64u;
constexpr std::uint16_t kMaxRelPathBytes = 512u;
constexpr std::uint32_t kMaxArchiveTotalEntries = 262144u;
constexpr std::uint32_t kMaxPatchedEntriesPerArchive = 65536u;
constexpr std::uint32_t kMaxSinglePayloadBytes = 64u * 1024u * 1024u;
constexpr std::size_t kStreamChunkBytes = 64u * 1024u;
constexpr std::size_t kArchiveTempFilesPerSection = 2u;
constexpr std::size_t kInitialModulePathCapacity = MAX_PATH;
constexpr std::size_t kMaxModulePathCapacity = 32768u;

constexpr std::size_t kByteIndex0 = 0u;
constexpr std::size_t kByteIndex1 = 1u;
constexpr std::size_t kByteIndex2 = 2u;
constexpr std::size_t kByteIndex3 = 3u;
constexpr std::size_t kByteIndex4 = 4u;
constexpr std::size_t kByteIndex5 = 5u;
constexpr std::size_t kByteIndex6 = 6u;
constexpr std::size_t kByteIndex7 = 7u;

constexpr unsigned int kShiftByte1 = 8u;
constexpr unsigned int kShiftByte2 = 16u;
constexpr unsigned int kShiftByte3 = 24u;
constexpr unsigned int kShiftByte4 = 32u;
constexpr unsigned int kShiftByte5 = 40u;
constexpr unsigned int kShiftByte6 = 48u;
constexpr unsigned int kShiftByte7 = 56u;

constexpr std::uint32_t kMurmurMultiplier = 0x5BD1E995u;
constexpr int kMurmurShiftR = 24;
constexpr int kMurmurFinalShift1 = 13;
constexpr int kMurmurFinalShift2 = 15;
constexpr std::size_t kWordBytes = 4u;
constexpr std::size_t kRemainingThree = 3u;
constexpr std::size_t kRemainingTwo = 2u;
constexpr std::size_t kRemainingOne = 1u;

constexpr std::uint32_t kXorStreamMultiplier = 0x9Eu;
constexpr std::uint32_t kXorStreamAddend = 0x37u;
constexpr std::size_t kHashByteIndexMask = 3u;
constexpr unsigned int kBitsPerByte = 8u;
constexpr std::uint32_t kLowByteMask = 0xFFu;
constexpr std::uint64_t kLowU32Mask = 0xFFFFFFFFull;
constexpr unsigned char kMinPrintableAscii = 0x20u;
constexpr unsigned char kAsciiHighBit = 0x80u;

class ScopedNamedMutex {
public:
    explicit ScopedNamedMutex(const wchar_t* name) noexcept
        : handle_(CreateMutexW(nullptr, FALSE, name)) {
        if (handle_ != nullptr) {
            const DWORD wait_res = WaitForSingleObject(handle_, INFINITE);
            locked_ = (wait_res == WAIT_OBJECT_0 || wait_res == WAIT_ABANDONED);
        }
    }

    ScopedNamedMutex(const ScopedNamedMutex&) = delete;
    ScopedNamedMutex& operator=(const ScopedNamedMutex&) = delete;

    ~ScopedNamedMutex() {
        if (handle_ != nullptr) {
            if (locked_) {
                ReleaseMutex(handle_);
            }
            CloseHandle(handle_);
        }
    }

    bool IsValid() const noexcept { return handle_ != nullptr && locked_; }

private:
    HANDLE handle_ = nullptr;
    bool locked_ = false;
};

class ScopedFileHandle {
public:
    ScopedFileHandle() noexcept = default;
    explicit ScopedFileHandle(HANDLE handle) noexcept : handle_(handle) {}

    ScopedFileHandle(const ScopedFileHandle&) = delete;
    ScopedFileHandle& operator=(const ScopedFileHandle&) = delete;

    ~ScopedFileHandle() { Reset(); }

    HANDLE Get() const noexcept { return handle_; }

    bool IsValid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    void Reset(HANDLE new_handle = INVALID_HANDLE_VALUE) noexcept {
        if (IsValid()) {
            CloseHandle(handle_);
        }
        handle_ = new_handle;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

std::uint16_t ReadU16Le(const std::uint8_t* p) noexcept {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(p[kByteIndex0]) |
        (static_cast<std::uint16_t>(p[kByteIndex1]) << kShiftByte1));
}

std::uint32_t ReadU32Le(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[kByteIndex0]) |
           (static_cast<std::uint32_t>(p[kByteIndex1]) << kShiftByte1) |
           (static_cast<std::uint32_t>(p[kByteIndex2]) << kShiftByte2) |
           (static_cast<std::uint32_t>(p[kByteIndex3]) << kShiftByte3);
}

std::uint64_t ReadU64Le(const std::uint8_t* p) noexcept {
    return static_cast<std::uint64_t>(p[kByteIndex0]) |
           (static_cast<std::uint64_t>(p[kByteIndex1]) << kShiftByte1) |
           (static_cast<std::uint64_t>(p[kByteIndex2]) << kShiftByte2) |
           (static_cast<std::uint64_t>(p[kByteIndex3]) << kShiftByte3) |
           (static_cast<std::uint64_t>(p[kByteIndex4]) << kShiftByte4) |
           (static_cast<std::uint64_t>(p[kByteIndex5]) << kShiftByte5) |
           (static_cast<std::uint64_t>(p[kByteIndex6]) << kShiftByte6) |
           (static_cast<std::uint64_t>(p[kByteIndex7]) << kShiftByte7);
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& rel) {
    if (dir.empty()) {
        return rel;
    }
    const wchar_t back = dir.back();
    if (back == L'\\' || back == L'/') {
        return dir + rel;
    }
    return dir + L"\\" + rel;
}

bool ValidateRelativeSubPath(const char* ascii_path,
                             std::size_t len,
                             std::wstring& out_wide) {
    if (ascii_path == nullptr || len == 0 || len > kMaxRelPathBytes) {
        return false;
    }
    if (ascii_path[0] == '\\' || ascii_path[0] == '/' ||
        ascii_path[len - 1u] == '\\' || ascii_path[len - 1u] == '/') {
        return false;
    }

    out_wide.clear();
    out_wide.reserve(len);
    for (std::size_t i = 0; i < len; ++i) {
        const auto c = static_cast<unsigned char>(ascii_path[i]);
        if (c < kMinPrintableAscii || c >= kAsciiHighBit || c == ':') {
            return false;
        }
        if (c == '.' && i + 1u < len && ascii_path[i + 1u] == '.') {
            return false;
        }
        out_wide.push_back(c == '/' ? L'\\' : static_cast<wchar_t>(c));
    }
    return true;
}

bool GetFileSizeBytes(const std::wstring& path, std::uint64_t& out_size) noexcept {
    ScopedFileHandle file(CreateFileW(path.c_str(),
                                      FILE_READ_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr,
                                      OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL,
                                      nullptr));
    if (!file.IsValid()) {
        return false;
    }
    LARGE_INTEGER size_li{};
    if (!GetFileSizeEx(file.Get(), &size_li) || size_li.QuadPart < 0) {
        return false;
    }
    out_size = static_cast<std::uint64_t>(size_li.QuadPart);
    return true;
}

bool FileExists(const std::wstring& path) noexcept {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0u;
}

bool ReadExactBytes(HANDLE hFile, void* dst, std::size_t byte_count) noexcept {
    if (byte_count == 0) {
        return true;
    }
    auto* cursor = static_cast<std::uint8_t*>(dst);
    std::size_t remaining = byte_count;
    while (remaining > 0) {
        const auto chunk = static_cast<DWORD>(
            remaining > kMaxSinglePayloadBytes ? kMaxSinglePayloadBytes : remaining);
        DWORD bytes_read = 0;
        if (!ReadFile(hFile, cursor, chunk, &bytes_read, nullptr) || bytes_read != chunk) {
            return false;
        }
        cursor += bytes_read;
        remaining -= bytes_read;
    }
    return true;
}

bool WriteExactBytes(HANDLE hFile, const void* src, std::size_t byte_count) noexcept {
    if (byte_count == 0) {
        return true;
    }
    const auto* cursor = static_cast<const std::uint8_t*>(src);
    std::size_t remaining = byte_count;
    while (remaining > 0) {
        const auto chunk = static_cast<DWORD>(
            remaining > kMaxSinglePayloadBytes ? kMaxSinglePayloadBytes : remaining);
        DWORD bytes_written = 0;
        if (!WriteFile(hFile, cursor, chunk, &bytes_written, nullptr) ||
            bytes_written != chunk) {
            return false;
        }
        cursor += bytes_written;
        remaining -= bytes_written;
    }
    return true;
}

bool SeekFileAbs(HANDLE hFile, std::uint64_t offset) noexcept {
    LARGE_INTEGER pos{};
    pos.QuadPart = static_cast<LONGLONG>(offset);
    return SetFilePointerEx(hFile, pos, nullptr, FILE_BEGIN) != FALSE;
}

bool StreamCopyRange(HANDLE src_file,
                     std::uint64_t src_offset,
                     std::uint64_t byte_count,
                     HANDLE dst_file,
                     Bytes& chunk_buf) {
    if (byte_count == 0) {
        return true;
    }
    if (!SeekFileAbs(src_file, src_offset)) {
        return false;
    }
    if (chunk_buf.size() < kStreamChunkBytes) {
        chunk_buf.resize(kStreamChunkBytes);
    }

    std::uint64_t remaining = byte_count;
    while (remaining > 0) {
        const std::size_t to_copy =
            (remaining > kStreamChunkBytes)
                ? kStreamChunkBytes
                : static_cast<std::size_t>(remaining);
        if (!ReadExactBytes(src_file, chunk_buf.data(), to_copy)) {
            return false;
        }
        if (!WriteExactBytes(dst_file, chunk_buf.data(), to_copy)) {
            return false;
        }
        remaining -= to_copy;
    }
    return true;
}

bool ReadWholeFile(const std::wstring& path, Bytes& out_bytes) {
    std::uint64_t file_size = 0;
    if (!GetFileSizeBytes(path, file_size) || file_size > kMaxSinglePayloadBytes) {
        return false;
    }

    ScopedFileHandle file(CreateFileW(path.c_str(),
                                      GENERIC_READ,
                                      FILE_SHARE_READ,
                                      nullptr,
                                      OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL,
                                      nullptr));
    if (!file.IsValid()) {
        return false;
    }

    out_bytes.resize(static_cast<std::size_t>(file_size));
    return ReadExactBytes(file.Get(), out_bytes.data(), out_bytes.size());
}

bool WriteWholeFile(const std::wstring& path, const Bytes& bytes) {
    ScopedFileHandle file(CreateFileW(path.c_str(),
                                      GENERIC_WRITE,
                                      0,
                                      nullptr,
                                      CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL,
                                      nullptr));
    if (!file.IsValid()) {
        return false;
    }
    if (!WriteExactBytes(file.Get(), bytes.data(), bytes.size())) {
        file.Reset();
        DeleteFileW(path.c_str());
        return false;
    }
    if (!FlushFileBuffers(file.Get())) {
        file.Reset();
        DeleteFileW(path.c_str());
        return false;
    }
    return true;
}

struct PatchHeader {
    std::uint32_t version = 0;
    std::uint32_t standalone_group_count = 0;
    std::uint32_t archive_count = 0;
    std::uint32_t flags = 0;
    std::uint64_t reserved = 0;
};

struct StandaloneVariantMeta {
    std::uint32_t orig_file_size = 0;
    std::uint32_t orig_data_hash = 0;
    std::uint32_t new_file_size = 0;
    std::uint32_t new_data_hash = 0;
    std::uint64_t payload_offset = 0;
};

struct StandaloneGroupMeta {
    std::wstring rel_path;
    std::wstring target_path;
    std::wstring bak_path;
    std::wstring tmp_path;
    std::vector<StandaloneVariantMeta> variants;
    const StandaloneVariantMeta* matched_variant = nullptr;
    bool use_bak_as_source = false;
    bool already_patched = false;
};

struct ArchiveEntryMeta {
    std::uint32_t entry_index = 0;
    std::uint64_t orig_offset = 0;
    std::uint32_t orig_size = 0;
    std::uint32_t orig_data_hash = 0;
    std::uint32_t new_size = 0;
    std::uint32_t new_data_hash = 0;
    std::uint64_t payload_offset = 0;
};

struct ArchiveSectionMeta {
    std::wstring stem_rel_path;
    std::wstring gpk_path;
    std::wstring gpk_bak_path;
    std::wstring gpk_tmp_path;
    std::wstring gtb_path;
    std::wstring gtb_bak_path;
    std::wstring gtb_tmp_path;
    std::uint64_t orig_gpk_size = 0;
    std::uint64_t new_gpk_size = 0;
    std::uint32_t orig_gtb_size = 0;
    std::uint32_t orig_gtb_hash = 0;
    std::uint32_t new_gtb_size = 0;
    std::uint32_t new_gtb_hash = 0;
    std::uint64_t gtb_payload_offset = 0;
    std::vector<ArchiveEntryMeta> entries;
    bool already_patched = false;
};

struct ParsedGtbOffsets {
    std::uint32_t entry_count = 0;
    bool has_over2g_table = false;
    std::vector<std::uint64_t> offsets;
};

void CleanupTempFiles(const std::vector<std::wstring>& temp_paths) noexcept {
    for (const std::wstring& p : temp_paths) {
        if (!p.empty() && FileExists(p)) {
            DeleteFileW(p.c_str());
        }
    }
}

bool ContainsDuplicateWidePath(const std::vector<StandaloneGroupMeta>& groups,
                               const std::wstring& candidate) {
    for (const StandaloneGroupMeta& grp : groups) {
        if (grp.rel_path == candidate) {
            return true;
        }
    }
    return false;
}

bool ContainsDuplicateArchiveStem(const std::vector<ArchiveSectionMeta>& sections,
                                  const std::wstring& candidate) {
    for (const ArchiveSectionMeta& sec : sections) {
        if (sec.stem_rel_path == candidate) {
            return true;
        }
    }
    return false;
}

bool ParsePatchHeader(HANDLE patch_file, PatchHeader& out_hdr) noexcept {
    std::uint8_t raw[kPatchHeaderSize] = {};
    if (!ReadExactBytes(patch_file, raw, kPatchHeaderSize)) {
        return false;
    }
    if (std::memcmp(raw, kPatchMagic, kPatchMagicSize) != 0) {
        return false;
    }

    out_hdr.version = ReadU32Le(raw + kHeaderOffsetVersion);
    out_hdr.standalone_group_count = ReadU32Le(raw + kHeaderOffsetStandaloneCount);
    out_hdr.archive_count = ReadU32Le(raw + kHeaderOffsetArchiveCount);
    out_hdr.flags = ReadU32Le(raw + kHeaderOffsetFlags);
    out_hdr.reserved = ReadU64Le(raw + kHeaderOffsetReserved);

    if (out_hdr.version != kPatchFormatVersion ||
        out_hdr.flags != 0u ||
        out_hdr.reserved != 0ull) {
        return false;
    }
    if (out_hdr.standalone_group_count > kMaxStandaloneGroups ||
        out_hdr.archive_count > kMaxArchiveSections) {
        return false;
    }
    return (out_hdr.standalone_group_count > 0u || out_hdr.archive_count > 0u);
}

bool ScanStandaloneGroups(HANDLE patch_file,
                          std::uint64_t patch_file_size,
                          const std::wstring& game_dir,
                          std::uint32_t group_count,
                          std::uint64_t& io_offset,
                          std::vector<StandaloneGroupMeta>& out_groups) {
    out_groups.clear();
    out_groups.reserve(group_count);
    std::vector<char> path_buf;

    for (std::uint32_t g = 0; g < group_count; ++g) {
        if (kU16FieldBytes > patch_file_size - io_offset) {
            return false;
        }
        std::uint8_t path_len_raw[kU16FieldBytes] = {};
        if (!ReadExactBytes(patch_file, path_len_raw, kU16FieldBytes)) {
            return false;
        }
        io_offset += kU16FieldBytes;

        const std::uint16_t path_len = ReadU16Le(path_len_raw);
        if (path_len == 0u || path_len > kMaxRelPathBytes ||
            static_cast<std::uint64_t>(path_len) + kU16FieldBytes >
                patch_file_size - io_offset) {
            return false;
        }

        path_buf.resize(path_len);
        if (!ReadExactBytes(patch_file, path_buf.data(), path_len)) {
            return false;
        }
        io_offset += path_len;

        StandaloneGroupMeta grp;
        if (!ValidateRelativeSubPath(path_buf.data(), path_len, grp.rel_path) ||
            ContainsDuplicateWidePath(out_groups, grp.rel_path)) {
            return false;
        }

        std::uint8_t var_count_raw[kU16FieldBytes] = {};
        if (!ReadExactBytes(patch_file, var_count_raw, kU16FieldBytes)) {
            return false;
        }
        io_offset += kU16FieldBytes;

        const std::uint16_t variant_count = ReadU16Le(var_count_raw);
        if (variant_count == 0u || variant_count > kMaxVariantsPerGroup) {
            return false;
        }

        grp.target_path = JoinPath(game_dir, grp.rel_path);
        grp.bak_path = grp.target_path + kBackupSuffix;
        grp.tmp_path = grp.target_path + kTempSuffix;
        grp.variants.reserve(variant_count);

        for (std::uint16_t v = 0; v < variant_count; ++v) {
            if (kStandaloneVariantHeaderBytes > patch_file_size - io_offset) {
                return false;
            }
            std::uint8_t var_raw[kStandaloneVariantHeaderBytes] = {};
            if (!ReadExactBytes(patch_file, var_raw, kStandaloneVariantHeaderBytes)) {
                return false;
            }
            io_offset += kStandaloneVariantHeaderBytes;

            StandaloneVariantMeta var;
            var.orig_file_size = ReadU32Le(var_raw + kVariantOrigSizeOffset);
            var.orig_data_hash = ReadU32Le(var_raw + kVariantOrigHashOffset);
            var.new_file_size = ReadU32Le(var_raw + kVariantNewSizeOffset);
            var.new_data_hash = ReadU32Le(var_raw + kVariantNewHashOffset);
            var.payload_offset = io_offset;

            if (var.orig_file_size == 0u || var.new_file_size == 0u ||
                var.orig_file_size > kMaxSinglePayloadBytes ||
                var.new_file_size > kMaxSinglePayloadBytes ||
                var.new_file_size > patch_file_size - io_offset) {
                return false;
            }

            io_offset += var.new_file_size;
            if (!SeekFileAbs(patch_file, io_offset)) {
                return false;
            }
            grp.variants.push_back(var);
        }

        out_groups.push_back(std::move(grp));
    }
    return true;
}

bool ScanArchiveSections(HANDLE patch_file,
                         std::uint64_t patch_file_size,
                         const std::wstring& game_dir,
                         std::uint32_t archive_count,
                         std::uint64_t& io_offset,
                         std::vector<ArchiveSectionMeta>& out_archives) {
    out_archives.clear();
    out_archives.reserve(archive_count);
    std::vector<char> stem_buf;

    for (std::uint32_t a = 0; a < archive_count; ++a) {
        if (kU16FieldBytes > patch_file_size - io_offset) {
            return false;
        }
        std::uint8_t stem_len_raw[kU16FieldBytes] = {};
        if (!ReadExactBytes(patch_file, stem_len_raw, kU16FieldBytes)) {
            return false;
        }
        io_offset += kU16FieldBytes;

        const std::uint16_t stem_len = ReadU16Le(stem_len_raw);
        if (stem_len == 0u || stem_len > kMaxRelPathBytes ||
            static_cast<std::uint64_t>(stem_len) + kArchiveFixedHeaderBytes >
                patch_file_size - io_offset) {
            return false;
        }

        stem_buf.resize(stem_len);
        if (!ReadExactBytes(patch_file, stem_buf.data(), stem_len)) {
            return false;
        }
        io_offset += stem_len;

        ArchiveSectionMeta sec;
        if (!ValidateRelativeSubPath(stem_buf.data(), stem_len, sec.stem_rel_path) ||
            ContainsDuplicateArchiveStem(out_archives, sec.stem_rel_path)) {
            return false;
        }

        std::uint8_t fixed_raw[kArchiveFixedHeaderBytes] = {};
        if (!ReadExactBytes(patch_file, fixed_raw, kArchiveFixedHeaderBytes)) {
            return false;
        }
        io_offset += kArchiveFixedHeaderBytes;

        sec.orig_gpk_size = ReadU64Le(fixed_raw + kArchiveOrigGpkSizeOffset);
        sec.new_gpk_size = ReadU64Le(fixed_raw + kArchiveNewGpkSizeOffset);
        sec.orig_gtb_size = ReadU32Le(fixed_raw + kArchiveOrigGtbSizeOffset);
        sec.orig_gtb_hash = ReadU32Le(fixed_raw + kArchiveOrigGtbHashOffset);
        sec.new_gtb_size = ReadU32Le(fixed_raw + kArchiveNewGtbSizeOffset);
        sec.new_gtb_hash = ReadU32Le(fixed_raw + kArchiveNewGtbHashOffset);
        const std::uint32_t entry_count = ReadU32Le(fixed_raw + kArchiveEntryCountOffset);

        if (sec.orig_gpk_size == 0ull || sec.new_gpk_size == 0ull ||
            sec.orig_gtb_size < kGtbCountFieldBytes ||
            sec.orig_gtb_size > kMaxSinglePayloadBytes ||
            sec.new_gtb_size != sec.orig_gtb_size ||
            entry_count > kMaxPatchedEntriesPerArchive ||
            sec.new_gtb_size > patch_file_size - io_offset) {
            return false;
        }

        const std::wstring base_abs = JoinPath(game_dir, sec.stem_rel_path);
        sec.gpk_path = base_abs + kGpkExtension;
        sec.gpk_bak_path = sec.gpk_path + kBackupSuffix;
        sec.gpk_tmp_path = sec.gpk_path + kTempSuffix;
        sec.gtb_path = base_abs + kGtbExtension;
        sec.gtb_bak_path = sec.gtb_path + kBackupSuffix;
        sec.gtb_tmp_path = sec.gtb_path + kTempSuffix;

        sec.gtb_payload_offset = io_offset;
        io_offset += sec.new_gtb_size;
        if (!SeekFileAbs(patch_file, io_offset)) {
            return false;
        }

        sec.entries.reserve(entry_count);
        for (std::uint32_t e = 0; e < entry_count; ++e) {
            if (kArchiveEntryHeaderBytes > patch_file_size - io_offset) {
                return false;
            }
            std::uint8_t entry_raw[kArchiveEntryHeaderBytes] = {};
            if (!ReadExactBytes(patch_file, entry_raw, kArchiveEntryHeaderBytes)) {
                return false;
            }
            io_offset += kArchiveEntryHeaderBytes;

            ArchiveEntryMeta rec;
            rec.entry_index = ReadU32Le(entry_raw + kEntryIndexOffset);
            rec.orig_offset = ReadU64Le(entry_raw + kEntryOrigOffsetOffset);
            rec.orig_size = ReadU32Le(entry_raw + kEntryOrigSizeOffset);
            rec.orig_data_hash = ReadU32Le(entry_raw + kEntryOrigHashOffset);
            rec.new_size = ReadU32Le(entry_raw + kEntryNewSizeOffset);
            rec.new_data_hash = ReadU32Le(entry_raw + kEntryNewHashOffset);
            rec.payload_offset = io_offset;

            if ((e > 0u && rec.entry_index <= sec.entries.back().entry_index) ||
                rec.orig_size == 0u || rec.new_size == 0u ||
                rec.orig_size > kMaxSinglePayloadBytes ||
                rec.new_size > kMaxSinglePayloadBytes ||
                rec.orig_offset > sec.orig_gpk_size ||
                rec.orig_size > sec.orig_gpk_size - rec.orig_offset ||
                rec.new_size > patch_file_size - io_offset) {
                return false;
            }

            io_offset += rec.new_size;
            if (!SeekFileAbs(patch_file, io_offset)) {
                return false;
            }
            sec.entries.push_back(rec);
        }

        out_archives.push_back(std::move(sec));
    }
    return true;
}

const StandaloneVariantMeta* MatchStandaloneVariantByOrig(
    const std::vector<StandaloneVariantMeta>& variants,
    const Bytes& candidate_bytes) noexcept {
    const std::uint32_t hash =
        ComputeMurmurHash2(candidate_bytes.data(), candidate_bytes.size());
    for (const StandaloneVariantMeta& var : variants) {
        if (candidate_bytes.size() == var.orig_file_size && hash == var.orig_data_hash) {
            return &var;
        }
    }
    return nullptr;
}

bool ResolveStandaloneGroupState(StandaloneGroupMeta& grp) {
    Bytes bak_bytes;
    if (FileExists(grp.bak_path) && ReadWholeFile(grp.bak_path, bak_bytes)) {
        const StandaloneVariantMeta* var =
            MatchStandaloneVariantByOrig(grp.variants, bak_bytes);
        if (var != nullptr) {
            grp.matched_variant = var;
            grp.use_bak_as_source = true;

            std::uint64_t cur_target_sz = 0;
            Bytes target_bytes;
            if (GetFileSizeBytes(grp.target_path, cur_target_sz) &&
                cur_target_sz == var->new_file_size &&
                ReadWholeFile(grp.target_path, target_bytes) &&
                ComputeMurmurHash2(target_bytes.data(), target_bytes.size()) ==
                    var->new_data_hash) {
                grp.already_patched = true;
            }
            return true;
        }
    }

    Bytes target_bytes;
    if (!ReadWholeFile(grp.target_path, target_bytes)) {
        return false;
    }
    const StandaloneVariantMeta* var =
        MatchStandaloneVariantByOrig(grp.variants, target_bytes);
    if (var == nullptr) {
        return false;
    }
    grp.matched_variant = var;
    grp.use_bak_as_source = false;
    grp.already_patched = false;
    return true;
}

bool IsArchiveSectionAlreadyPatched(const ArchiveSectionMeta& sec) {
    std::uint64_t cur_gpk_sz = 0;
    std::uint64_t bak_gpk_sz = 0;
    std::uint64_t cur_gtb_sz = 0;
    std::uint64_t bak_gtb_sz = 0;
    if (!GetFileSizeBytes(sec.gpk_path, cur_gpk_sz) || cur_gpk_sz != sec.new_gpk_size) {
        return false;
    }
    if (!GetFileSizeBytes(sec.gpk_bak_path, bak_gpk_sz) ||
        bak_gpk_sz != sec.orig_gpk_size) {
        return false;
    }
    if (!GetFileSizeBytes(sec.gtb_path, cur_gtb_sz) || cur_gtb_sz != sec.new_gtb_size) {
        return false;
    }
    if (!GetFileSizeBytes(sec.gtb_bak_path, bak_gtb_sz) ||
        bak_gtb_sz != sec.orig_gtb_size) {
        return false;
    }

    Bytes gtb_bytes;
    if (!ReadWholeFile(sec.gtb_path, gtb_bytes)) {
        return false;
    }
    return ComputeMurmurHash2(gtb_bytes.data(), gtb_bytes.size()) == sec.new_gtb_hash;
}

bool ParseGtbOffsetsTable(const Bytes& gtb_bytes,
                          std::uint64_t expected_gpk_size,
                          ParsedGtbOffsets& out_table) {
    if (gtb_bytes.size() < kGtbCountFieldBytes) {
        return false;
    }
    const std::uint32_t count = ReadU32Le(gtb_bytes.data());
    if (count == 0u || count > kMaxArchiveTotalEntries) {
        return false;
    }

    const std::size_t base_tables_end =
        kGtbCountFieldBytes +
        static_cast<std::size_t>(count) * kGtbU32EntryStride * kGtbU32TableCount;
    if (gtb_bytes.size() < base_tables_end) {
        return false;
    }

    out_table.entry_count = count;
    out_table.offsets.assign(static_cast<std::size_t>(count) + 1u, 0ull);

    const std::size_t u32_off_pos =
        kGtbCountFieldBytes + static_cast<std::size_t>(count) * kGtbU32EntryStride;
    const std::size_t min_over2g_size =
        base_tables_end + static_cast<std::size_t>(count) * kGtbU64EntryStride +
        kGtbOver2GFooterPaddingBytes;

    const bool has_over2g =
        gtb_bytes.size() >= min_over2g_size &&
        std::memcmp(gtb_bytes.data() + (gtb_bytes.size() - kGtbOver2GTrailerSize),
                    kGtbOver2GTrailer,
                    kGtbOver2GTrailerSize) == 0;
    out_table.has_over2g_table = has_over2g;

    if (has_over2g) {
        const std::size_t u64_off_pos =
            gtb_bytes.size() - kGtbOver2GFooterPaddingBytes -
            static_cast<std::size_t>(count) * kGtbU64EntryStride;
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t off32 = ReadU32Le(
                gtb_bytes.data() + u32_off_pos + static_cast<std::size_t>(i) * kGtbU32EntryStride);
            const std::uint64_t off64 = ReadU64Le(
                gtb_bytes.data() + u64_off_pos + static_cast<std::size_t>(i) * kGtbU64EntryStride);
            if (static_cast<std::uint32_t>(off64 & kLowU32Mask) != off32) {
                return false;
            }
            out_table.offsets[i] = off64;
        }
    }
    if (!has_over2g) {
        for (std::uint32_t i = 0; i < count; ++i) {
            out_table.offsets[i] = ReadU32Le(
                gtb_bytes.data() + u32_off_pos + static_cast<std::size_t>(i) * kGtbU32EntryStride);
        }
    }

    out_table.offsets[count] = expected_gpk_size;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (out_table.offsets[i] > out_table.offsets[i + 1u]) {
            return false;
        }
    }
    return true;
}

bool ValidateArchiveOffsetTables(const ArchiveSectionMeta& sec,
                                 const ParsedGtbOffsets& orig_table,
                                 const ParsedGtbOffsets& new_table) noexcept {
    if (orig_table.entry_count != new_table.entry_count ||
        orig_table.has_over2g_table != new_table.has_over2g_table) {
        return false;
    }

    std::size_t mod_cursor = 0;
    std::uint64_t running_new_off = 0ull;
    for (std::uint32_t idx = 0; idx < orig_table.entry_count; ++idx) {
        if (new_table.offsets[idx] != running_new_off) {
            return false;
        }
        const std::uint64_t orig_off = orig_table.offsets[idx];
        const std::uint64_t orig_sz = orig_table.offsets[idx + 1u] - orig_off;

        if (mod_cursor < sec.entries.size() &&
            sec.entries[mod_cursor].entry_index == idx) {
            const ArchiveEntryMeta& rec = sec.entries[mod_cursor++];
            if (rec.orig_offset != orig_off || rec.orig_size != orig_sz) {
                return false;
            }
            running_new_off += rec.new_size;
            continue;
        }
        running_new_off += orig_sz;
    }

    return mod_cursor == sec.entries.size() && running_new_off == sec.new_gpk_size;
}

bool StageStandaloneTempFile(HANDLE patch_file, const StandaloneGroupMeta& grp) {
    if (grp.matched_variant == nullptr) {
        return false;
    }
    const StandaloneVariantMeta& var = *grp.matched_variant;
    const std::wstring& src_path = grp.use_bak_as_source ? grp.bak_path : grp.target_path;

    Bytes orig_bytes;
    if (!ReadWholeFile(src_path, orig_bytes) ||
        orig_bytes.size() != var.orig_file_size ||
        ComputeMurmurHash2(orig_bytes.data(), orig_bytes.size()) != var.orig_data_hash) {
        return false;
    }

    if (!SeekFileAbs(patch_file, var.payload_offset)) {
        return false;
    }
    Bytes patched_bytes(var.new_file_size);
    if (!ReadExactBytes(patch_file, patched_bytes.data(), patched_bytes.size())) {
        return false;
    }

    XorMaskPayload(patched_bytes.data(),
                   patched_bytes.size(),
                   orig_bytes.data(),
                   orig_bytes.size(),
                   var.orig_data_hash);
    if (ComputeMurmurHash2(patched_bytes.data(), patched_bytes.size()) !=
        var.new_data_hash) {
        return false;
    }

    return WriteWholeFile(grp.tmp_path, patched_bytes);
}

bool SelectOriginalArchivePaths(const ArchiveSectionMeta& sec,
                                Bytes& out_orig_gtb,
                                std::wstring& out_orig_gpk_path) {
    std::uint64_t bak_gtb_sz = 0;
    const bool use_gtb_bak =
        GetFileSizeBytes(sec.gtb_bak_path, bak_gtb_sz) && bak_gtb_sz == sec.orig_gtb_size;
    const std::wstring& gtb_src = use_gtb_bak ? sec.gtb_bak_path : sec.gtb_path;
    if (!ReadWholeFile(gtb_src, out_orig_gtb) ||
        out_orig_gtb.size() != sec.orig_gtb_size ||
        ComputeMurmurHash2(out_orig_gtb.data(), out_orig_gtb.size()) !=
            sec.orig_gtb_hash) {
        return false;
    }

    std::uint64_t bak_gpk_sz = 0;
    if (GetFileSizeBytes(sec.gpk_bak_path, bak_gpk_sz) &&
        bak_gpk_sz == sec.orig_gpk_size) {
        out_orig_gpk_path = sec.gpk_bak_path;
        return true;
    }
    std::uint64_t cur_gpk_sz = 0;
    if (GetFileSizeBytes(sec.gpk_path, cur_gpk_sz) &&
        cur_gpk_sz == sec.orig_gpk_size) {
        out_orig_gpk_path = sec.gpk_path;
        return true;
    }
    return false;
}

bool StageArchiveTempFiles(HANDLE patch_file,
                           const ArchiveSectionMeta& sec,
                           Bytes& stream_buf) {
    Bytes orig_gtb;
    std::wstring orig_gpk_path;
    if (!SelectOriginalArchivePaths(sec, orig_gtb, orig_gpk_path)) {
        return false;
    }

    if (!SeekFileAbs(patch_file, sec.gtb_payload_offset)) {
        return false;
    }
    Bytes new_gtb(sec.new_gtb_size);
    if (!ReadExactBytes(patch_file, new_gtb.data(), new_gtb.size())) {
        return false;
    }
    XorMaskPayload(new_gtb.data(),
                   new_gtb.size(),
                   orig_gtb.data(),
                   orig_gtb.size(),
                   sec.orig_gtb_hash);
    if (ComputeMurmurHash2(new_gtb.data(), new_gtb.size()) != sec.new_gtb_hash) {
        return false;
    }

    ParsedGtbOffsets orig_table;
    ParsedGtbOffsets new_table;
    if (!ParseGtbOffsetsTable(orig_gtb, sec.orig_gpk_size, orig_table) ||
        !ParseGtbOffsetsTable(new_gtb, sec.new_gpk_size, new_table) ||
        !ValidateArchiveOffsetTables(sec, orig_table, new_table)) {
        return false;
    }

    ScopedFileHandle orig_gpk_file(CreateFileW(orig_gpk_path.c_str(),
                                               GENERIC_READ,
                                               FILE_SHARE_READ,
                                               nullptr,
                                               OPEN_EXISTING,
                                               FILE_FLAG_SEQUENTIAL_SCAN,
                                               nullptr));
    if (!orig_gpk_file.IsValid()) {
        return false;
    }

    ScopedFileHandle gpk_tmp_file(CreateFileW(sec.gpk_tmp_path.c_str(),
                                              GENERIC_WRITE,
                                              0,
                                              nullptr,
                                              CREATE_ALWAYS,
                                              FILE_ATTRIBUTE_NORMAL,
                                              nullptr));
    if (!gpk_tmp_file.IsValid()) {
        return false;
    }

    Bytes orig_entry;
    Bytes new_entry;
    std::uint64_t orig_cursor = 0ull;
    std::uint64_t total_written = 0ull;

    for (const ArchiveEntryMeta& rec : sec.entries) {
        if (rec.orig_offset < orig_cursor) {
            return false;
        }
        const std::uint64_t untouched_len = rec.orig_offset - orig_cursor;
        if (!StreamCopyRange(orig_gpk_file.Get(),
                             orig_cursor,
                             untouched_len,
                             gpk_tmp_file.Get(),
                             stream_buf)) {
            return false;
        }
        total_written += untouched_len;

        if (!SeekFileAbs(orig_gpk_file.Get(), rec.orig_offset)) {
            return false;
        }
        orig_entry.resize(rec.orig_size);
        if (!ReadExactBytes(orig_gpk_file.Get(), orig_entry.data(), rec.orig_size) ||
            ComputeMurmurHash2(orig_entry.data(), orig_entry.size()) !=
                rec.orig_data_hash) {
            return false;
        }

        if (!SeekFileAbs(patch_file, rec.payload_offset)) {
            return false;
        }
        new_entry.resize(rec.new_size);
        if (!ReadExactBytes(patch_file, new_entry.data(), rec.new_size)) {
            return false;
        }

        XorMaskPayload(new_entry.data(),
                       new_entry.size(),
                       orig_entry.data(),
                       orig_entry.size(),
                       rec.orig_data_hash);
        if (ComputeMurmurHash2(new_entry.data(), new_entry.size()) != rec.new_data_hash) {
            return false;
        }

        if (!WriteExactBytes(gpk_tmp_file.Get(), new_entry.data(), new_entry.size())) {
            return false;
        }
        total_written += new_entry.size();
        orig_cursor = rec.orig_offset + rec.orig_size;
    }

    if (orig_cursor > sec.orig_gpk_size) {
        return false;
    }
    const std::uint64_t tail_len = sec.orig_gpk_size - orig_cursor;
    if (!StreamCopyRange(
            orig_gpk_file.Get(), orig_cursor, tail_len, gpk_tmp_file.Get(), stream_buf)) {
        return false;
    }
    total_written += tail_len;

    if (total_written != sec.new_gpk_size) {
        return false;
    }
    if (!FlushFileBuffers(gpk_tmp_file.Get())) {
        return false;
    }
    gpk_tmp_file.Reset();
    orig_gpk_file.Reset();

    return WriteWholeFile(sec.gtb_tmp_path, new_gtb);
}

bool CommitPatchedFile(const std::wstring& target_path,
                       const std::wstring& bak_path,
                       const std::wstring& tmp_path,
                       std::uint64_t orig_size) {
    std::uint64_t existing_bak_size = 0;
    const bool valid_bak_exists =
        GetFileSizeBytes(bak_path, existing_bak_size) && existing_bak_size == orig_size;
    if (!valid_bak_exists) {
        if (FileExists(bak_path)) {
            DeleteFileW(bak_path.c_str());
        }
        if (!CopyFileW(target_path.c_str(), bak_path.c_str(), FALSE)) {
            return false;
        }
    }

    return MoveFileExW(tmp_path.c_str(),
                       target_path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

bool ResolveSelfModuleDirectory(std::wstring& out_dir) {
    HMODULE self_mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&EnsureGameFilesPatched),
                            &self_mod)) {
        self_mod = GetModuleHandleW(nullptr);
    }

    std::size_t cap = kInitialModulePathCapacity;
    while (cap <= kMaxModulePathCapacity) {
        std::wstring buf(cap, L'\0');
        const DWORD len =
            GetModuleFileNameW(self_mod, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0u) {
            return false;
        }
        if (len < buf.size()) {
            buf.resize(len);
            const std::size_t slash_pos = buf.find_last_of(L"\\/");
            if (slash_pos == std::wstring::npos) {
                return false;
            }
            buf.resize(slash_pos);
            out_dir = std::move(buf);
            return true;
        }
        cap *= 2u;
    }
    return false;
}

}  // namespace

std::uint32_t ComputeMurmurHash2(const std::uint8_t* data,
                                 std::size_t len,
                                 std::uint32_t seed) noexcept {
    if (data == nullptr) {
        len = 0u;
    }
    std::uint32_t h = seed ^ static_cast<std::uint32_t>(len);
    std::size_t i = 0;
    while (i + kWordBytes <= len) {
        std::uint32_t k = ReadU32Le(data + i);
        k *= kMurmurMultiplier;
        k ^= k >> kMurmurShiftR;
        k *= kMurmurMultiplier;
        h *= kMurmurMultiplier;
        h ^= k;
        i += kWordBytes;
    }

    const std::size_t rem = len - i;
    if (rem == kRemainingThree) {
        h ^= static_cast<std::uint32_t>(data[i + kByteIndex2]) << kShiftByte2;
    }
    if (rem >= kRemainingTwo) {
        h ^= static_cast<std::uint32_t>(data[i + kByteIndex1]) << kShiftByte1;
    }
    if (rem >= kRemainingOne) {
        h ^= static_cast<std::uint32_t>(data[i + kByteIndex0]);
        h *= kMurmurMultiplier;
    }

    h ^= h >> kMurmurFinalShift1;
    h *= kMurmurMultiplier;
    h ^= h >> kMurmurFinalShift2;
    return h;
}

void XorMaskPayload(std::uint8_t* buf,
                    std::size_t buf_len,
                    const std::uint8_t* orig_slice,
                    std::size_t orig_len,
                    std::uint32_t orig_data_hash) noexcept {
    if (buf == nullptr || buf_len == 0u) {
        return;
    }

    const bool has_orig = (orig_slice != nullptr && orig_len > 0u);
    for (std::size_t i = 0; i < buf_len; ++i) {
        const std::uint8_t key_byte = has_orig ? orig_slice[i % orig_len] : 0u;
        const auto shift =
            static_cast<unsigned int>((i & kHashByteIndexMask) * kBitsPerByte);
        const auto pos_mix = static_cast<std::uint8_t>(
            ((orig_data_hash >> shift) & kLowByteMask) ^
            (static_cast<std::uint32_t>(i) * kXorStreamMultiplier + kXorStreamAddend));
        buf[i] ^= static_cast<std::uint8_t>(key_byte ^ pos_mix);
    }
}

bool EnsureGameFilesPatchedInDir(const std::wstring& game_dir) {
    if (game_dir.empty()) {
        return false;
    }

    ScopedNamedMutex mutex_lock(kPatchMutexName);
    if (!mutex_lock.IsValid()) {
        return false;
    }

    const std::wstring patch_path = JoinPath(game_dir, kPatchBinFileName);
    if (!FileExists(patch_path)) {
        return false;
    }

    std::uint64_t patch_file_size = 0;
    if (!GetFileSizeBytes(patch_path, patch_file_size) ||
        patch_file_size < kPatchHeaderSize) {
        Log("MenuAutopatch: menu_patch.bin invalid or truncated");
        return false;
    }

    ScopedFileHandle patch_file(CreateFileW(patch_path.c_str(),
                                            GENERIC_READ,
                                            FILE_SHARE_READ,
                                            nullptr,
                                            OPEN_EXISTING,
                                            FILE_ATTRIBUTE_NORMAL,
                                            nullptr));
    if (!patch_file.IsValid()) {
        Log("MenuAutopatch: failed to open menu_patch.bin");
        return false;
    }

    PatchHeader hdr{};
    if (!ParsePatchHeader(patch_file.Get(), hdr)) {
        Log("MenuAutopatch: menu_patch.bin header check failed");
        return false;
    }

    std::uint64_t scan_offset = kPatchHeaderSize;
    std::vector<StandaloneGroupMeta> standalone_groups;
    if (!ScanStandaloneGroups(patch_file.Get(),
                              patch_file_size,
                              game_dir,
                              hdr.standalone_group_count,
                              scan_offset,
                              standalone_groups)) {
        Log("MenuAutopatch: standalone metadata scan failed");
        return false;
    }

    std::vector<ArchiveSectionMeta> archives;
    if (!ScanArchiveSections(patch_file.Get(),
                             patch_file_size,
                             game_dir,
                             hdr.archive_count,
                             scan_offset,
                             archives) ||
        scan_offset != patch_file_size) {
        Log("MenuAutopatch: archive metadata scan or EOF check failed");
        return false;
    }

    bool all_standalone_patched = true;
    for (StandaloneGroupMeta& grp : standalone_groups) {
        if (!ResolveStandaloneGroupState(grp)) {
            Log("MenuAutopatch: unmatched or missing standalone file");
            return false;
        }
        if (!grp.already_patched) {
            all_standalone_patched = false;
        }
    }

    bool all_archives_patched = true;
    for (ArchiveSectionMeta& sec : archives) {
        sec.already_patched = IsArchiveSectionAlreadyPatched(sec);
        if (!sec.already_patched) {
            all_archives_patched = false;
        }
    }

    if (all_standalone_patched && all_archives_patched) {
        Log("MenuAutopatch: already up-to-date (%zu standalone files, %zu archives)",
            standalone_groups.size(),
            archives.size());
        return true;
    }

    std::vector<std::wstring> staged_temps;
    staged_temps.reserve(standalone_groups.size() +
                         archives.size() * kArchiveTempFilesPerSection);

    for (const StandaloneGroupMeta& grp : standalone_groups) {
        if (grp.already_patched) {
            continue;
        }
        staged_temps.push_back(grp.tmp_path);
        if (!StageStandaloneTempFile(patch_file.Get(), grp)) {
            CleanupTempFiles(staged_temps);
            Log("MenuAutopatch: standalone staging failed, rolled back temp files");
            return false;
        }
    }

    Bytes stream_buf;
    std::size_t patched_entry_total = 0;
    for (const ArchiveSectionMeta& sec : archives) {
        if (sec.already_patched) {
            continue;
        }
        staged_temps.push_back(sec.gpk_tmp_path);
        staged_temps.push_back(sec.gtb_tmp_path);
        if (!StageArchiveTempFiles(patch_file.Get(), sec, stream_buf)) {
            CleanupTempFiles(staged_temps);
            Log("MenuAutopatch: archive staging failed, rolled back temp files");
            return false;
        }
        patched_entry_total += sec.entries.size();
    }

    for (const StandaloneGroupMeta& grp : standalone_groups) {
        if (grp.already_patched) {
            continue;
        }
        if (!CommitPatchedFile(grp.target_path,
                               grp.bak_path,
                               grp.tmp_path,
                               grp.matched_variant->orig_file_size)) {
            CleanupTempFiles(staged_temps);
            Log("MenuAutopatch: standalone commit failed");
            return false;
        }
    }

    for (const ArchiveSectionMeta& sec : archives) {
        if (sec.already_patched) {
            continue;
        }
        if (!CommitPatchedFile(
                sec.gpk_path, sec.gpk_bak_path, sec.gpk_tmp_path, sec.orig_gpk_size) ||
            !CommitPatchedFile(
                sec.gtb_path, sec.gtb_bak_path, sec.gtb_tmp_path, sec.orig_gtb_size)) {
            CleanupTempFiles(staged_temps);
            Log("MenuAutopatch: archive commit failed");
            return false;
        }
    }

    Log("MenuAutopatch: patched %zu standalone files and %zu GPK entries across %zu archives (.bak backups verified)",
        standalone_groups.size(),
        patched_entry_total,
        archives.size());
    return true;
}

bool EnsureGameFilesPatched() {
    std::wstring dir;
    if (!ResolveSelfModuleDirectory(dir)) {
        return false;
    }
    return EnsureGameFilesPatchedInDir(dir);
}

}  // namespace menu_autopatch
