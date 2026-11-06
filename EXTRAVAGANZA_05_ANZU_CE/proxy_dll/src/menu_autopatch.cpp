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
#include <vector>

namespace menu_autopatch {

namespace {

using Bytes = std::vector<std::uint8_t>;

constexpr wchar_t kPatchMutexName[] = L"Local\\AnzUCeMenuPatchMutex";
constexpr wchar_t kPatchBinFileName[] = L"menu_patch.bin";
constexpr wchar_t kSysGpkRelPath[] = L"dwq\\sys0.gpk";
constexpr wchar_t kSysGtbRelPath[] = L"dwq\\sys0.gtb";
constexpr wchar_t kBackupSuffix[] = L".bak";
constexpr wchar_t kTempSuffix[] = L".tmp";

constexpr std::size_t kPatchMagicSize = 8u;
constexpr char kPatchMagic[kPatchMagicSize] = {'A', 'N', 'Z', 'M', 'E', 'N', 'U', '1'};
constexpr std::size_t kPatchHeaderSize = 44u;
constexpr std::uint32_t kPatchFormatVersion = 1u;

constexpr std::size_t kHeaderOffsetVersion = 8u;
constexpr std::size_t kHeaderOffsetStandaloneCount = 12u;
constexpr std::size_t kHeaderOffsetOrigGpkSize = 16u;
constexpr std::size_t kHeaderOffsetNewGpkSize = 20u;
constexpr std::size_t kHeaderOffsetOrigGtbSize = 24u;
constexpr std::size_t kHeaderOffsetOrigGtbHash = 28u;
constexpr std::size_t kHeaderOffsetNewGtbSize = 32u;
constexpr std::size_t kHeaderOffsetNewGtbHash = 36u;
constexpr std::size_t kHeaderOffsetGpkEntryCount = 40u;

constexpr std::size_t kRelPathLenFieldBytes = 2u;
constexpr std::size_t kStandaloneFixedMetaBytes = 16u;
constexpr std::size_t kStandaloneOrigSizeOffset = 0u;
constexpr std::size_t kStandaloneOrigHashOffset = 4u;
constexpr std::size_t kStandaloneNewSizeOffset = 8u;
constexpr std::size_t kStandaloneNewHashOffset = 12u;

constexpr std::size_t kGpkEntryFixedMetaBytes = 24u;
constexpr std::size_t kGpkEntryIndexOffset = 0u;
constexpr std::size_t kGpkOrigOffsetOffset = 4u;
constexpr std::size_t kGpkOrigSizeOffset = 8u;
constexpr std::size_t kGpkOrigHashOffset = 12u;
constexpr std::size_t kGpkNewSizeOffset = 16u;
constexpr std::size_t kGpkNewHashOffset = 20u;

constexpr std::size_t kGtbCountFieldBytes = 4u;
constexpr std::size_t kGtbOffsetTableCount = 2u;
constexpr std::size_t kGtbOffsetElemBytes = 4u;

constexpr std::uint32_t kMaxStandaloneFiles = 256u;
constexpr std::uint16_t kMaxRelPathBytes = 512u;
constexpr std::uint32_t kMaxGpkEntries = 4096u;
constexpr std::uint32_t kMaxSinglePayloadBytes = 128u * 1024u * 1024u;

constexpr std::uint32_t kMurmurMultiplier = 0x5BD1E995u;
constexpr int kMurmurShiftR = 24;
constexpr int kMurmurFinalShift1 = 13;
constexpr int kMurmurFinalShift2 = 15;
constexpr std::size_t kWordBytes = 4u;
constexpr std::size_t kRemainingThree = 3u;
constexpr std::size_t kRemainingTwo = 2u;
constexpr std::size_t kRemainingOne = 1u;
constexpr std::size_t kOffsetTwo = 2u;
constexpr int kShiftByte1 = 8;
constexpr int kShiftByte2 = 16;
constexpr int kShiftByte3 = 24;

constexpr std::uint32_t kXorStreamMultiplier = 0x9Eu;
constexpr std::uint32_t kXorStreamAddend = 0x37u;
constexpr std::size_t kHashByteIndexMask = 3u;
constexpr unsigned int kBitsPerByte = 8u;
constexpr std::uint32_t kLowByteMask = 0xFFu;
constexpr int kHighDwordShift = 32;
constexpr unsigned char kAsciiHighBit = 0x80u;

class ScopedNamedMutex {
public:
    explicit ScopedNamedMutex(const wchar_t* name) noexcept
        : handle_(CreateMutexW(nullptr, FALSE, name))
    {
        if (handle_ != nullptr) {
            const DWORD wait_res = WaitForSingleObject(handle_, INFINITE);
            locked_ = (wait_res == WAIT_OBJECT_0 || wait_res == WAIT_ABANDONED);
        }
    }

    ~ScopedNamedMutex()
    {
        if (handle_ != nullptr) {
            if (locked_) {
                ReleaseMutex(handle_);
            }
            CloseHandle(handle_);
        }
    }

    ScopedNamedMutex(const ScopedNamedMutex&) = delete;
    ScopedNamedMutex& operator=(const ScopedNamedMutex&) = delete;

    bool IsValid() const noexcept { return handle_ != nullptr && locked_; }

private:
    HANDLE handle_ = nullptr;
    bool locked_ = false;
};

class ScopedFileHandle {
public:
    ScopedFileHandle() noexcept = default;
    explicit ScopedFileHandle(HANDLE handle) noexcept : handle_(handle) {}

    ~ScopedFileHandle() { Reset(); }

    ScopedFileHandle(const ScopedFileHandle&) = delete;
    ScopedFileHandle& operator=(const ScopedFileHandle&) = delete;

    HANDLE Get() const noexcept { return handle_; }

    bool IsValid() const noexcept
    {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    void Reset(HANDLE new_handle = INVALID_HANDLE_VALUE) noexcept
    {
        if (IsValid()) {
            CloseHandle(handle_);
        }
        handle_ = new_handle;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

std::uint16_t ReadU16Le(const std::uint8_t* p) noexcept
{
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(p[0]) |
        (static_cast<std::uint16_t>(p[1]) << kShiftByte1));
}

std::uint32_t ReadU32Le(const std::uint8_t* p) noexcept
{
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << kShiftByte1) |
           (static_cast<std::uint32_t>(p[kOffsetTwo]) << kShiftByte2) |
           (static_cast<std::uint32_t>(p[kRemainingThree]) << kShiftByte3);
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& rel)
{
    if (dir.empty()) {
        return rel;
    }
    const wchar_t back = dir.back();
    if (back == L'\\' || back == L'/') {
        return dir + rel;
    }
    return dir + L"\\" + rel;
}

bool ConvertRelPathToWide(const char* utf8, std::size_t len, std::wstring& out)
{
    if (utf8 == nullptr || len == 0 || len > kMaxRelPathBytes) {
        return false;
    }
    if (utf8[0] == '\\' || utf8[0] == '/') {
        return false;
    }

    out.clear();
    out.reserve(len);
    for (std::size_t i = 0; i < len; ++i) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        if (c == 0u || c == ':' || c >= kAsciiHighBit) {
            return false;
        }
        if (c == '.' && i + 1u < len && utf8[i + 1u] == '.') {
            return false;
        }
        out.push_back(c == '/' ? L'\\' : static_cast<wchar_t>(c));
    }
    return true;
}

bool GetFileSizeBytes(const std::wstring& path, std::uint64_t& out_size) noexcept
{
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        return false;
    }
    if ((fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u) {
        return false;
    }
    out_size = (static_cast<std::uint64_t>(fad.nFileSizeHigh) << kHighDwordShift) |
               static_cast<std::uint64_t>(fad.nFileSizeLow);
    return true;
}

bool FileExists(const std::wstring& path) noexcept
{
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0u;
}

bool ReadExactBytes(HANDLE hFile, void* dst, std::size_t byte_count) noexcept
{
    if (byte_count == 0) {
        return true;
    }
    auto* cursor = static_cast<std::uint8_t*>(dst);
    std::size_t remaining = byte_count;
    while (remaining > 0) {
        const DWORD chunk = static_cast<DWORD>(
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

bool WriteExactBytes(HANDLE hFile, const void* src, std::size_t byte_count) noexcept
{
    if (byte_count == 0) {
        return true;
    }
    const auto* cursor = static_cast<const std::uint8_t*>(src);
    std::size_t remaining = byte_count;
    while (remaining > 0) {
        const DWORD chunk = static_cast<DWORD>(
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

bool SeekFileAbs(HANDLE hFile, std::uint64_t offset) noexcept
{
    LARGE_INTEGER pos{};
    pos.QuadPart = static_cast<LONGLONG>(offset);
    return SetFilePointerEx(hFile, pos, nullptr, FILE_BEGIN) != FALSE;
}

bool ReadWholeFile(const std::wstring& path, Bytes& out_bytes)
{
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

bool WriteWholeFile(const std::wstring& path, const Bytes& bytes)
{
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
    return FlushFileBuffers(file.Get()) != FALSE;
}

bool IsTargetFileAlreadyPatched(const std::wstring& target_path,
                                const std::wstring& bak_path,
                                std::uint32_t orig_size,
                                std::uint32_t new_size,
                                std::uint32_t new_hash)
{
    std::uint64_t cur_target_size = 0;
    std::uint64_t cur_bak_size = 0;
    if (!GetFileSizeBytes(target_path, cur_target_size) || cur_target_size != new_size) {
        return false;
    }
    if (!GetFileSizeBytes(bak_path, cur_bak_size) || cur_bak_size != orig_size) {
        return false;
    }

    if (orig_size == new_size) {
        Bytes current_bytes;
        if (!ReadWholeFile(target_path, current_bytes)) {
            return false;
        }
        return ComputeMurmurHash2(current_bytes.data(), current_bytes.size()) == new_hash;
    }
    return true;
}

bool CommitPatchedFile(const std::wstring& target_path,
                       const std::wstring& bak_path,
                       const std::wstring& tmp_path,
                       std::uint32_t orig_size)
{
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

    if (!MoveFileExW(tmp_path.c_str(),
                     target_path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return false;
    }
    return true;
}

struct PatchHeader {
    std::uint32_t version = 0;
    std::uint32_t standalone_file_count = 0;
    std::uint32_t orig_gpk_size = 0;
    std::uint32_t new_gpk_size = 0;
    std::uint32_t orig_gtb_size = 0;
    std::uint32_t orig_gtb_hash = 0;
    std::uint32_t new_gtb_size = 0;
    std::uint32_t new_gtb_hash = 0;
    std::uint32_t gpk_entry_count = 0;
};

struct StandaloneFileMeta {
    std::wstring rel_path;
    std::wstring target_path;
    std::wstring bak_path;
    std::wstring tmp_path;
    std::uint32_t orig_file_size = 0;
    std::uint32_t orig_data_hash = 0;
    std::uint32_t new_file_size = 0;
    std::uint32_t new_data_hash = 0;
    std::uint64_t payload_offset = 0;
    bool already_patched = false;
};

struct GpkEntryMeta {
    std::uint32_t entry_index = 0;
    std::uint32_t orig_offset = 0;
    std::uint32_t orig_size = 0;
    std::uint32_t orig_data_hash = 0;
    std::uint32_t new_size = 0;
    std::uint32_t new_data_hash = 0;
    std::uint64_t payload_offset = 0;
};

void CleanupTempFiles(const std::vector<std::wstring>& temp_paths) noexcept
{
    for (const std::wstring& p : temp_paths) {
        if (!p.empty() && FileExists(p)) {
            DeleteFileW(p.c_str());
        }
    }
}

bool ParsePatchHeader(HANDLE patch_file, PatchHeader& out_hdr) noexcept
{
    std::uint8_t raw[kPatchHeaderSize] = {};
    if (!ReadExactBytes(patch_file, raw, kPatchHeaderSize)) {
        return false;
    }
    if (std::memcmp(raw, kPatchMagic, kPatchMagicSize) != 0) {
        return false;
    }

    out_hdr.version = ReadU32Le(raw + kHeaderOffsetVersion);
    out_hdr.standalone_file_count = ReadU32Le(raw + kHeaderOffsetStandaloneCount);
    out_hdr.orig_gpk_size = ReadU32Le(raw + kHeaderOffsetOrigGpkSize);
    out_hdr.new_gpk_size = ReadU32Le(raw + kHeaderOffsetNewGpkSize);
    out_hdr.orig_gtb_size = ReadU32Le(raw + kHeaderOffsetOrigGtbSize);
    out_hdr.orig_gtb_hash = ReadU32Le(raw + kHeaderOffsetOrigGtbHash);
    out_hdr.new_gtb_size = ReadU32Le(raw + kHeaderOffsetNewGtbSize);
    out_hdr.new_gtb_hash = ReadU32Le(raw + kHeaderOffsetNewGtbHash);
    out_hdr.gpk_entry_count = ReadU32Le(raw + kHeaderOffsetGpkEntryCount);

    if (out_hdr.version != kPatchFormatVersion) {
        return false;
    }
    if (out_hdr.standalone_file_count > kMaxStandaloneFiles ||
        out_hdr.gpk_entry_count > kMaxGpkEntries) {
        return false;
    }
    if (out_hdr.orig_gpk_size == 0 || out_hdr.new_gpk_size == 0 ||
        out_hdr.orig_gtb_size < kGtbCountFieldBytes ||
        out_hdr.new_gtb_size < kGtbCountFieldBytes ||
        out_hdr.orig_gtb_size > kMaxSinglePayloadBytes ||
        out_hdr.new_gtb_size > kMaxSinglePayloadBytes) {
        return false;
    }
    return true;
}

bool ScanStandaloneMetas(HANDLE patch_file,
                         std::uint64_t patch_file_size,
                         const std::wstring& game_dir,
                         std::uint32_t standalone_count,
                         std::vector<StandaloneFileMeta>& out_metas,
                         std::uint64_t& out_next_offset)
{
    out_metas.clear();
    out_metas.reserve(standalone_count);

    std::uint64_t cur_offset = kPatchHeaderSize;
    std::vector<char> path_buf;

    for (std::uint32_t i = 0; i < standalone_count; ++i) {
        if (cur_offset + kRelPathLenFieldBytes > patch_file_size) {
            return false;
        }
        std::uint8_t len_bytes[kRelPathLenFieldBytes] = {};
        if (!ReadExactBytes(patch_file, len_bytes, kRelPathLenFieldBytes)) {
            return false;
        }
        cur_offset += kRelPathLenFieldBytes;

        const std::uint16_t path_len = ReadU16Le(len_bytes);
        if (path_len == 0 || path_len > kMaxRelPathBytes ||
            cur_offset + path_len + kStandaloneFixedMetaBytes > patch_file_size) {
            return false;
        }

        path_buf.resize(path_len);
        if (!ReadExactBytes(patch_file, path_buf.data(), path_len)) {
            return false;
        }
        cur_offset += path_len;

        StandaloneFileMeta meta;
        if (!ConvertRelPathToWide(path_buf.data(), path_len, meta.rel_path)) {
            return false;
        }

        std::uint8_t fixed_meta[kStandaloneFixedMetaBytes] = {};
        if (!ReadExactBytes(patch_file, fixed_meta, kStandaloneFixedMetaBytes)) {
            return false;
        }
        cur_offset += kStandaloneFixedMetaBytes;

        meta.orig_file_size = ReadU32Le(fixed_meta + kStandaloneOrigSizeOffset);
        meta.orig_data_hash = ReadU32Le(fixed_meta + kStandaloneOrigHashOffset);
        meta.new_file_size = ReadU32Le(fixed_meta + kStandaloneNewSizeOffset);
        meta.new_data_hash = ReadU32Le(fixed_meta + kStandaloneNewHashOffset);
        meta.payload_offset = cur_offset;

        if (meta.orig_file_size == 0 || meta.new_file_size == 0 ||
            meta.orig_file_size > kMaxSinglePayloadBytes ||
            meta.new_file_size > kMaxSinglePayloadBytes ||
            cur_offset + meta.new_file_size > patch_file_size) {
            return false;
        }

        meta.target_path = JoinPath(game_dir, meta.rel_path);
        meta.bak_path = meta.target_path + kBackupSuffix;
        meta.tmp_path = meta.target_path + kTempSuffix;

        cur_offset += meta.new_file_size;
        if (!SeekFileAbs(patch_file, cur_offset)) {
            return false;
        }

        out_metas.push_back(std::move(meta));
    }

    out_next_offset = cur_offset;
    return true;
}

bool ScanGpkEntryMetas(HANDLE patch_file,
                       std::uint64_t patch_file_size,
                       std::uint64_t section_c_offset,
                       std::uint32_t gpk_entry_count,
                       std::uint32_t archive_total_entries,
                       std::vector<GpkEntryMeta>& out_metas)
{
    out_metas.clear();
    out_metas.reserve(gpk_entry_count);

    std::uint64_t cur_offset = section_c_offset;
    if (!SeekFileAbs(patch_file, cur_offset)) {
        return false;
    }

    for (std::uint32_t i = 0; i < gpk_entry_count; ++i) {
        if (cur_offset + kGpkEntryFixedMetaBytes > patch_file_size) {
            return false;
        }
        std::uint8_t raw[kGpkEntryFixedMetaBytes] = {};
        if (!ReadExactBytes(patch_file, raw, kGpkEntryFixedMetaBytes)) {
            return false;
        }
        cur_offset += kGpkEntryFixedMetaBytes;

        GpkEntryMeta meta;
        meta.entry_index = ReadU32Le(raw + kGpkEntryIndexOffset);
        meta.orig_offset = ReadU32Le(raw + kGpkOrigOffsetOffset);
        meta.orig_size = ReadU32Le(raw + kGpkOrigSizeOffset);
        meta.orig_data_hash = ReadU32Le(raw + kGpkOrigHashOffset);
        meta.new_size = ReadU32Le(raw + kGpkNewSizeOffset);
        meta.new_data_hash = ReadU32Le(raw + kGpkNewHashOffset);
        meta.payload_offset = cur_offset;

        if (meta.entry_index >= archive_total_entries ||
            (i > 0 && meta.entry_index <= out_metas.back().entry_index) ||
            meta.orig_size == 0 || meta.new_size == 0 ||
            meta.orig_size > kMaxSinglePayloadBytes ||
            meta.new_size > kMaxSinglePayloadBytes ||
            cur_offset + meta.new_size > patch_file_size) {
            return false;
        }

        cur_offset += meta.new_size;
        if (!SeekFileAbs(patch_file, cur_offset)) {
            return false;
        }

        out_metas.push_back(meta);
    }

    return cur_offset == patch_file_size;
}

bool StageStandaloneTempFile(HANDLE patch_file, const StandaloneFileMeta& meta)
{
    std::uint64_t bak_size = 0;
    const std::wstring& src_path =
        (GetFileSizeBytes(meta.bak_path, bak_size) && bak_size == meta.orig_file_size)
            ? meta.bak_path
            : meta.target_path;

    Bytes orig_bytes;
    if (!ReadWholeFile(src_path, orig_bytes) ||
        orig_bytes.size() != meta.orig_file_size ||
        ComputeMurmurHash2(orig_bytes.data(), orig_bytes.size()) != meta.orig_data_hash) {
        return false;
    }

    if (!SeekFileAbs(patch_file, meta.payload_offset)) {
        return false;
    }
    Bytes patched_bytes(meta.new_file_size);
    if (!ReadExactBytes(patch_file, patched_bytes.data(), patched_bytes.size())) {
        return false;
    }

    XorMaskPayload(patched_bytes.data(),
                   patched_bytes.size(),
                   orig_bytes.data(),
                   orig_bytes.size(),
                   meta.orig_data_hash);
    if (ComputeMurmurHash2(patched_bytes.data(), patched_bytes.size()) !=
        meta.new_data_hash) {
        return false;
    }

    return WriteWholeFile(meta.tmp_path, patched_bytes);
}

bool StageArchiveTempFiles(HANDLE patch_file,
                           std::uint64_t patch_file_size,
                           const PatchHeader& hdr,
                           std::uint64_t gtb_payload_offset,
                           const std::wstring& gpk_path,
                           const std::wstring& gpk_bak_path,
                           const std::wstring& gpk_tmp_path,
                           const std::wstring& gtb_path,
                           const std::wstring& gtb_bak_path,
                           const std::wstring& gtb_tmp_path)
{
    std::uint64_t bak_gtb_size = 0;
    const std::wstring& orig_gtb_path =
        (GetFileSizeBytes(gtb_bak_path, bak_gtb_size) && bak_gtb_size == hdr.orig_gtb_size)
            ? gtb_bak_path
            : gtb_path;

    Bytes orig_gtb;
    if (!ReadWholeFile(orig_gtb_path, orig_gtb) ||
        orig_gtb.size() != hdr.orig_gtb_size ||
        ComputeMurmurHash2(orig_gtb.data(), orig_gtb.size()) != hdr.orig_gtb_hash) {
        return false;
    }

    if (!SeekFileAbs(patch_file, gtb_payload_offset)) {
        return false;
    }
    Bytes new_gtb(hdr.new_gtb_size);
    if (!ReadExactBytes(patch_file, new_gtb.data(), new_gtb.size())) {
        return false;
    }
    XorMaskPayload(
        new_gtb.data(), new_gtb.size(), orig_gtb.data(), orig_gtb.size(), hdr.orig_gtb_hash);
    if (ComputeMurmurHash2(new_gtb.data(), new_gtb.size()) != hdr.new_gtb_hash) {
        return false;
    }

    const std::uint32_t entry_count = ReadU32Le(orig_gtb.data());
    const std::size_t tables_end =
        kGtbCountFieldBytes +
        static_cast<std::size_t>(entry_count) * kGtbOffsetTableCount * kGtbOffsetElemBytes;
    if (entry_count == 0 || entry_count > kMaxGpkEntries ||
        orig_gtb.size() < tables_end || new_gtb.size() < tables_end ||
        ReadU32Le(new_gtb.data()) != entry_count) {
        return false;
    }

    const std::uint64_t section_c_offset = gtb_payload_offset + hdr.new_gtb_size;
    std::vector<GpkEntryMeta> gpk_metas;
    if (!ScanGpkEntryMetas(patch_file,
                           patch_file_size,
                           section_c_offset,
                           hdr.gpk_entry_count,
                           entry_count,
                           gpk_metas)) {
        return false;
    }

    std::uint64_t bak_gpk_size = 0;
    std::uint64_t cur_gpk_size = 0;
    std::wstring orig_gpk_path;
    if (GetFileSizeBytes(gpk_bak_path, bak_gpk_size) && bak_gpk_size == hdr.orig_gpk_size) {
        orig_gpk_path = gpk_bak_path;
    } else if (GetFileSizeBytes(gpk_path, cur_gpk_size) && cur_gpk_size == hdr.orig_gpk_size) {
        orig_gpk_path = gpk_path;
    } else {
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

    ScopedFileHandle gpk_tmp_file(CreateFileW(gpk_tmp_path.c_str(),
                                              GENERIC_WRITE,
                                              0,
                                              nullptr,
                                              CREATE_ALWAYS,
                                              FILE_ATTRIBUTE_NORMAL,
                                              nullptr));
    if (!gpk_tmp_file.IsValid()) {
        return false;
    }

    Bytes orig_slice;
    Bytes new_slice;
    std::size_t mod_cursor = 0;
    std::uint64_t total_gpk_written = 0;

    for (std::uint32_t i = 0; i < entry_count; ++i) {
        const std::uint32_t orig_off = ReadU32Le(
            orig_gtb.data() + kGtbCountFieldBytes + (entry_count + i) * kGtbOffsetElemBytes);
        const std::uint32_t next_off =
            (i + 1u < entry_count)
                ? ReadU32Le(orig_gtb.data() + kGtbCountFieldBytes +
                            (entry_count + i + 1u) * kGtbOffsetElemBytes)
                : hdr.orig_gpk_size;
        if (orig_off > next_off || next_off > hdr.orig_gpk_size) {
            return false;
        }
        const std::uint32_t orig_sz = next_off - orig_off;
        if (orig_sz == 0 || orig_sz > kMaxSinglePayloadBytes) {
            return false;
        }

        if (!SeekFileAbs(orig_gpk_file.Get(), orig_off)) {
            return false;
        }
        orig_slice.resize(orig_sz);
        if (!ReadExactBytes(orig_gpk_file.Get(), orig_slice.data(), orig_sz)) {
            return false;
        }

        if (mod_cursor < gpk_metas.size() && gpk_metas[mod_cursor].entry_index == i) {
            const GpkEntryMeta& m = gpk_metas[mod_cursor++];
            if (orig_off != m.orig_offset || orig_sz != m.orig_size ||
                ComputeMurmurHash2(orig_slice.data(), orig_slice.size()) != m.orig_data_hash) {
                return false;
            }

            if (!SeekFileAbs(patch_file, m.payload_offset)) {
                return false;
            }
            new_slice.resize(m.new_size);
            if (!ReadExactBytes(patch_file, new_slice.data(), m.new_size)) {
                return false;
            }

            XorMaskPayload(new_slice.data(),
                           new_slice.size(),
                           orig_slice.data(),
                           orig_slice.size(),
                           m.orig_data_hash);
            if (ComputeMurmurHash2(new_slice.data(), new_slice.size()) != m.new_data_hash) {
                return false;
            }

            if (!WriteExactBytes(gpk_tmp_file.Get(), new_slice.data(), new_slice.size())) {
                return false;
            }
            total_gpk_written += new_slice.size();
        } else {
            if (!WriteExactBytes(gpk_tmp_file.Get(), orig_slice.data(), orig_slice.size())) {
                return false;
            }
            total_gpk_written += orig_slice.size();
        }
    }

    if (mod_cursor != gpk_metas.size() || total_gpk_written != hdr.new_gpk_size) {
        return false;
    }
    if (!FlushFileBuffers(gpk_tmp_file.Get())) {
        return false;
    }
    gpk_tmp_file.Reset();
    orig_gpk_file.Reset();

    return WriteWholeFile(gtb_tmp_path, new_gtb);
}

}  // namespace

std::uint32_t ComputeMurmurHash2(const std::uint8_t* data, std::size_t len) noexcept
{
    std::uint32_t h = static_cast<std::uint32_t>(len);
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
        h ^= static_cast<std::uint32_t>(data[i + kOffsetTwo]) << kShiftByte2;
    }
    if (rem >= kRemainingTwo) {
        h ^= static_cast<std::uint32_t>(data[i + kRemainingOne]) << kShiftByte1;
    }
    if (rem >= kRemainingOne) {
        h ^= static_cast<std::uint32_t>(data[i]);
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
                    std::uint32_t orig_data_hash) noexcept
{
    if (buf == nullptr || buf_len == 0 || orig_slice == nullptr || orig_len == 0) {
        return;
    }

    for (std::size_t i = 0; i < buf_len; ++i) {
        const std::uint8_t key_byte = orig_slice[i % orig_len];
        const unsigned int shift =
            static_cast<unsigned int>((i & kHashByteIndexMask) * kBitsPerByte);
        const std::uint8_t pos_mix = static_cast<std::uint8_t>(
            ((orig_data_hash >> shift) & kLowByteMask) ^
            (static_cast<std::uint32_t>(i) * kXorStreamMultiplier + kXorStreamAddend));
        buf[i] ^= static_cast<std::uint8_t>(key_byte ^ pos_mix);
    }
}

bool EnsureGameFilesPatchedInDir(const std::wstring& game_dir)
{
    if (game_dir.empty()) {
        return false;
    }

    ScopedNamedMutex mutex_lock(kPatchMutexName);
    if (!mutex_lock.IsValid()) {
        return false;
    }

    const std::wstring patch_path = JoinPath(game_dir, kPatchBinFileName);
    std::uint64_t patch_file_size = 0;
    if (!GetFileSizeBytes(patch_path, patch_file_size) ||
        patch_file_size < kPatchHeaderSize) {
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
        return false;
    }

    PatchHeader hdr{};
    if (!ParsePatchHeader(patch_file.Get(), hdr)) {
        return false;
    }

    std::vector<StandaloneFileMeta> standalone_metas;
    std::uint64_t gtb_payload_offset = 0;
    if (!ScanStandaloneMetas(patch_file.Get(),
                             patch_file_size,
                             game_dir,
                             hdr.standalone_file_count,
                             standalone_metas,
                             gtb_payload_offset)) {
        return false;
    }
    if (gtb_payload_offset + hdr.new_gtb_size > patch_file_size) {
        return false;
    }

    const std::wstring gpk_path = JoinPath(game_dir, kSysGpkRelPath);
    const std::wstring gpk_bak_path = gpk_path + kBackupSuffix;
    const std::wstring gpk_tmp_path = gpk_path + kTempSuffix;
    const std::wstring gtb_path = JoinPath(game_dir, kSysGtbRelPath);
    const std::wstring gtb_bak_path = gtb_path + kBackupSuffix;
    const std::wstring gtb_tmp_path = gtb_path + kTempSuffix;

    std::uint64_t cur_gpk_size = 0;
    std::uint64_t bak_gpk_size = 0;
    const bool gpk_already_patched =
        GetFileSizeBytes(gpk_path, cur_gpk_size) && cur_gpk_size == hdr.new_gpk_size &&
        GetFileSizeBytes(gpk_bak_path, bak_gpk_size) && bak_gpk_size == hdr.orig_gpk_size;
    const bool gtb_already_patched = IsTargetFileAlreadyPatched(
        gtb_path, gtb_bak_path, hdr.orig_gtb_size, hdr.new_gtb_size, hdr.new_gtb_hash);
    const bool archive_already_patched = gpk_already_patched && gtb_already_patched;

    bool all_standalone_patched = true;
    for (StandaloneFileMeta& meta : standalone_metas) {
        meta.already_patched = IsTargetFileAlreadyPatched(meta.target_path,
                                                          meta.bak_path,
                                                          meta.orig_file_size,
                                                          meta.new_file_size,
                                                          meta.new_data_hash);
        if (!meta.already_patched) {
            all_standalone_patched = false;
        }
    }

    if (archive_already_patched && all_standalone_patched) {
        return true;
    }

    std::vector<std::wstring> staged_temps;
    staged_temps.reserve(standalone_metas.size() + kGtbOffsetTableCount);

    for (const StandaloneFileMeta& meta : standalone_metas) {
        if (meta.already_patched) {
            continue;
        }
        staged_temps.push_back(meta.tmp_path);
        if (!StageStandaloneTempFile(patch_file.Get(), meta)) {
            CleanupTempFiles(staged_temps);
            return false;
        }
    }

    if (!archive_already_patched) {
        staged_temps.push_back(gpk_tmp_path);
        staged_temps.push_back(gtb_tmp_path);
        if (!StageArchiveTempFiles(patch_file.Get(),
                                   patch_file_size,
                                   hdr,
                                   gtb_payload_offset,
                                   gpk_path,
                                   gpk_bak_path,
                                   gpk_tmp_path,
                                   gtb_path,
                                   gtb_bak_path,
                                   gtb_tmp_path)) {
            CleanupTempFiles(staged_temps);
            return false;
        }
    }

    for (const StandaloneFileMeta& meta : standalone_metas) {
        if (meta.already_patched) {
            continue;
        }
        if (!CommitPatchedFile(
                meta.target_path, meta.bak_path, meta.tmp_path, meta.orig_file_size)) {
            CleanupTempFiles(staged_temps);
            return false;
        }
    }

    if (!archive_already_patched) {
        if (!CommitPatchedFile(gpk_path, gpk_bak_path, gpk_tmp_path, hdr.orig_gpk_size) ||
            !CommitPatchedFile(gtb_path, gtb_bak_path, gtb_tmp_path, hdr.orig_gtb_size)) {
            CleanupTempFiles(staged_temps);
            return false;
        }
    }

    Log("Auto-patched WelcomeToAnzU_CE game assets from menu_patch.bin");
    return true;
}

bool EnsureGameFilesPatched()
{
    HMODULE self_mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&EnsureGameFilesPatched),
                            &self_mod)) {
        self_mod = GetModuleHandleW(nullptr);
    }

    wchar_t mod_path[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(self_mod, mod_path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return false;
    }

    std::wstring dir(mod_path, len);
    const std::size_t slash_pos = dir.find_last_of(L"\\/");
    if (slash_pos == std::string::npos) {
        return false;
    }
    dir.resize(slash_pos);
    return EnsureGameFilesPatchedInDir(dir);
}

}  // namespace menu_autopatch
