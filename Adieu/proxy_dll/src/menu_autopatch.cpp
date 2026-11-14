// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "menu_autopatch.h"

#include "log.h"
#include "menu_patch_format.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace menu_autopatch {
namespace {

using menu_patch_format::AppliedSentinel;
using menu_patch_format::AppliedSentinelEntry;
using menu_patch_format::ByteSpan;
using menu_patch_format::kFlagBackupStandalone;
using menu_patch_format::kMaxAssetPayloadBytes;
using menu_patch_format::kPatchHeaderBytes;
using menu_patch_format::ManifestStatus;
using menu_patch_format::ManifestStatusName;
using menu_patch_format::MenuPatchHeader;
using menu_patch_format::MenuPatchManifest;
using menu_patch_format::MenuPatchRecord;
using menu_patch_format::MurmurHash2;
using menu_patch_format::ParseAppliedSentinel;
using menu_patch_format::ParseMenuPatchHeader;
using menu_patch_format::ParseMenuPatchManifest;
using menu_patch_format::SentinelStatus;
using menu_patch_format::SerializeAppliedSentinel;
using menu_patch_format::UnmaskRecordPayload;
using menu_patch_format::UnmaskStatus;
using menu_patch_format::UnmaskStatusName;
using menu_patch_format::Utf8PathToWide;

constexpr DWORD kMutexWaitTimeoutMs = 15000u;
constexpr unsigned int kHighDwordShift = 32u;
constexpr std::uint64_t kMaxPatchContainerBytes =
    512ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxSentinelFileBytes =
    4ULL * 1024ULL * 1024ULL;
constexpr DWORD kMaxIoChunkBytes = 64u * 1024u * 1024u;

constexpr wchar_t kStageTempSuffix[] = L".stage.tmp";
constexpr wchar_t kBackupSuffix[] = L".bak";
constexpr wchar_t kBackupTempSuffix[] = L".bak.tmp";
constexpr wchar_t kSentinelTempSuffix[] = L".tmp";

class ScopedWinHandle {
public:
    explicit ScopedWinHandle(HANDLE h = INVALID_HANDLE_VALUE) noexcept
        : handle_(h) {}

    ~ScopedWinHandle() {
        if (valid()) {
            ::CloseHandle(handle_);
        }
    }

    ScopedWinHandle(const ScopedWinHandle&) = delete;
    ScopedWinHandle& operator=(const ScopedWinHandle&) = delete;

    ScopedWinHandle(ScopedWinHandle&& other) noexcept
        : handle_(other.handle_) {
        other.handle_ = INVALID_HANDLE_VALUE;
    }

    ScopedWinHandle& operator=(ScopedWinHandle&& other) noexcept {
        if (this != &other) {
            if (valid()) {
                ::CloseHandle(handle_);
            }
            handle_ = other.handle_;
            other.handle_ = INVALID_HANDLE_VALUE;
        }
        return *this;
    }

    HANDLE get() const noexcept { return handle_; }
    bool valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_;
};

class ScopedMutexLock {
public:
    explicit ScopedMutexLock(const wchar_t* mutex_name) noexcept
        : mutex_(::CreateMutexW(nullptr, FALSE, mutex_name)) {
        if (!mutex_.valid()) {
            return;
        }
        const DWORD rc =
            ::WaitForSingleObject(mutex_.get(), kMutexWaitTimeoutMs);
        if (rc == WAIT_OBJECT_0 || rc == WAIT_ABANDONED) {
            locked_ = true;
        }
    }

    ~ScopedMutexLock() {
        if (locked_) {
            ::ReleaseMutex(mutex_.get());
        }
    }

    ScopedMutexLock(const ScopedMutexLock&) = delete;
    ScopedMutexLock& operator=(const ScopedMutexLock&) = delete;

    bool locked() const noexcept { return locked_; }

private:
    ScopedWinHandle mutex_;
    bool locked_ = false;
};

void ClearReadOnlyAttributeIfPresent(const std::wstring& path) noexcept {
    const DWORD attrs = ::GetFileAttributesW(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES &&
        (attrs & FILE_ATTRIBUTE_READONLY) != 0u) {
        ::SetFileAttributesW(path.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
    }
}

class ScopedTempFile {
public:
    ScopedTempFile() = default;

    ~ScopedTempFile() { Cleanup(); }

    ScopedTempFile(const ScopedTempFile&) = delete;
    ScopedTempFile& operator=(const ScopedTempFile&) = delete;

    ScopedTempFile(ScopedTempFile&& other) noexcept
        : temp_path_(std::move(other.temp_path_)), active_(other.active_) {
        other.active_ = false;
    }

    ScopedTempFile& operator=(ScopedTempFile&& other) noexcept {
        if (this != &other) {
            Cleanup();
            temp_path_ = std::move(other.temp_path_);
            active_ = other.active_;
            other.active_ = false;
        }
        return *this;
    }

    void Arm(std::wstring path) {
        Cleanup();
        temp_path_ = std::move(path);
        active_ = true;
    }

    bool CommitMove(const std::wstring& dest_path,
                    bool replace_existing) noexcept {
        if (!active_) {
            return false;
        }
        if (replace_existing) {
            ClearReadOnlyAttributeIfPresent(dest_path);
        }
        const DWORD flags = replace_existing
                                ? (MOVEFILE_REPLACE_EXISTING |
                                   MOVEFILE_WRITE_THROUGH)
                                : MOVEFILE_WRITE_THROUGH;
        if (!::MoveFileExW(temp_path_.c_str(), dest_path.c_str(), flags)) {
            return false;
        }
        active_ = false;
        return true;
    }

    void Cleanup() noexcept {
        if (!active_) {
            return;
        }
        ClearReadOnlyAttributeIfPresent(temp_path_);
        ::DeleteFileW(temp_path_.c_str());
        active_ = false;
    }

private:
    std::wstring temp_path_;
    bool active_ = false;
};

std::uint64_t PackFileSize64(DWORD high, DWORD low) noexcept {
    return (static_cast<std::uint64_t>(high) << kHighDwordShift) |
           static_cast<std::uint64_t>(low);
}

bool NormalizeGameDirWithTrailingSep(const wchar_t* game_dir,
                                     std::wstring& base_with_sep_out) {
    base_with_sep_out.clear();
    if (game_dir == nullptr || game_dir[0] == L'\0') {
        return false;
    }

    const DWORD attrs = ::GetFileAttributesW(game_dir);
    if (attrs == INVALID_FILE_ATTRIBUTES ||
        (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0u) {
        return false;
    }

    std::wstring normalized(game_dir);
    for (wchar_t& ch : normalized) {
        if (ch == L'/') {
            ch = L'\\';
        }
    }
    while (normalized.size() > 3u && normalized.back() == L'\\') {
        normalized.pop_back();
    }
    if (normalized.back() != L'\\') {
        normalized.push_back(L'\\');
    }
    base_with_sep_out = std::move(normalized);
    return true;
}

bool RegularFileExistsWin32(const std::wstring& path) noexcept {
    const DWORD attrs = ::GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0u;
}

enum class FileProbeResult {
    kAbsent,
    kFoundRegularFile,
    kError,
};

FileProbeResult ProbeRegularFileSize(const std::wstring& path,
                                     std::uint64_t& size_out) noexcept {
    size_out = 0u;
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr)) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            return FileProbeResult::kAbsent;
        }
        return FileProbeResult::kError;
    }
    if ((attr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u) {
        return FileProbeResult::kError;
    }
    size_out = PackFileSize64(attr.nFileSizeHigh, attr.nFileSizeLow);
    return FileProbeResult::kFoundRegularFile;
}

bool ReadFixedPrefixWin32(const std::wstring& path,
                          std::uint8_t* dst,
                          DWORD byte_count) noexcept {
    ScopedWinHandle file(::CreateFileW(
        path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) {
        return false;
    }

    DWORD total_read = 0u;
    while (total_read < byte_count) {
        DWORD got = 0u;
        if (!::ReadFile(file.get(), dst + total_read, byte_count - total_read,
                        &got, nullptr) ||
            got == 0u) {
            return false;
        }
        total_read += got;
    }
    return true;
}

bool ReadEntireFileWin32(const std::wstring& path,
                         std::uint64_t max_bytes,
                         std::vector<std::uint8_t>& bytes_out) {
    bytes_out.clear();
    ScopedWinHandle file(::CreateFileW(
        path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) {
        return false;
    }

    LARGE_INTEGER size_li{};
    if (!::GetFileSizeEx(file.get(), &size_li) || size_li.QuadPart < 0) {
        return false;
    }
    const auto file_size = static_cast<std::uint64_t>(size_li.QuadPart);
    if (file_size > max_bytes) {
        return false;
    }

    const auto total = static_cast<std::size_t>(file_size);
    std::vector<std::uint8_t> buf(total);
    std::size_t offset = 0u;
    while (offset < total) {
        const std::size_t rem = total - offset;
        const DWORD chunk = (rem > kMaxIoChunkBytes)
                                ? kMaxIoChunkBytes
                                : static_cast<DWORD>(rem);
        DWORD got = 0u;
        if (!::ReadFile(file.get(), buf.data() + offset, chunk, &got,
                        nullptr) ||
            got == 0u) {
            return false;
        }
        offset += static_cast<std::size_t>(got);
    }

    bytes_out = std::move(buf);
    return true;
}

bool EnsureRelativeParentDirsExist(const std::wstring& base_with_sep,
                                   std::wstring_view wide_rel_path) {
    std::size_t search_pos = 0u;
    for (;;) {
        const std::size_t sep = wide_rel_path.find(L'\\', search_pos);
        if (sep == std::wstring_view::npos) {
            return true;
        }
        const std::wstring subdir =
            base_with_sep + std::wstring(wide_rel_path.substr(0u, sep));
        if (!::CreateDirectoryW(subdir.c_str(), nullptr)) {
            const DWORD err = ::GetLastError();
            if (err != ERROR_ALREADY_EXISTS) {
                return false;
            }
        }
        search_pos = sep + 1u;
    }
}

bool StageTempFileWin32(const std::wstring& tmp_path,
                        ByteSpan contents,
                        ScopedTempFile& temp_guard_out) {
    ClearReadOnlyAttributeIfPresent(tmp_path);
    temp_guard_out.Arm(tmp_path);

    ScopedWinHandle file(::CreateFileW(tmp_path.c_str(), GENERIC_WRITE, 0u,
                                       nullptr, CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) {
        temp_guard_out.Cleanup();
        return false;
    }

    std::size_t offset = 0u;
    while (offset < contents.size()) {
        const std::size_t rem = contents.size() - offset;
        const DWORD chunk = (rem > kMaxIoChunkBytes)
                                ? kMaxIoChunkBytes
                                : static_cast<DWORD>(rem);
        DWORD written = 0u;
        if (!::WriteFile(file.get(), contents.data() + offset, chunk, &written,
                         nullptr) ||
            written != chunk) {
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }

    if (!::FlushFileBuffers(file.get())) {
        return false;
    }
    return true;
}

bool TryVerifyAlreadyAppliedFastPath(const std::wstring& base_with_sep,
                                     const std::wstring& patch_path,
                                     std::uint64_t actual_patch_size,
                                     const std::wstring& sentinel_path) {
    if (actual_patch_size < kPatchHeaderBytes) {
        return false;
    }

    std::vector<std::uint8_t> sentinel_bytes;
    if (!ReadEntireFileWin32(sentinel_path, kMaxSentinelFileBytes,
                             sentinel_bytes)) {
        return false;
    }

    AppliedSentinel sentinel{};
    if (ParseAppliedSentinel(ByteSpan(sentinel_bytes), sentinel) !=
        SentinelStatus::kOk) {
        return false;
    }
    if (sentinel.patch_file_size != actual_patch_size) {
        return false;
    }

    std::array<std::uint8_t, kPatchHeaderBytes> header_buf{};
    if (!ReadFixedPrefixWin32(patch_path, header_buf.data(),
                              static_cast<DWORD>(header_buf.size()))) {
        return false;
    }

    MenuPatchHeader header{};
    if (ParseMenuPatchHeader(ByteSpan(header_buf), header) !=
        ManifestStatus::kOk) {
        return false;
    }
    if (sentinel.manifest_hash != header.manifest_hash ||
        sentinel.entries.size() != header.record_count) {
        return false;
    }

    std::wstring wide_rel;
    for (const auto& entry : sentinel.entries) {
        if (!Utf8PathToWide(entry.rel_path, wide_rel)) {
            return false;
        }
        std::uint64_t actual_target_size = 0u;
        if (ProbeRegularFileSize(base_with_sep + wide_rel,
                                 actual_target_size) !=
                FileProbeResult::kFoundRegularFile ||
            actual_target_size != entry.expected_new_size) {
            return false;
        }
    }
    return true;
}

struct StagedRecord {
    std::wstring target_path;
    std::wstring backup_path;
    ScopedTempFile staged_target_tmp;
    ScopedTempFile staged_backup_tmp;
    bool needs_target_swap = false;
    bool needs_backup_commit = false;
};

bool StageSingleRecord(const std::wstring& base_with_sep,
                       ByteSpan patch_blob,
                       const MenuPatchRecord& rec,
                       StagedRecord& staged_out) {
    std::wstring wide_src_rel;
    std::wstring wide_dst_rel;
    if (!Utf8PathToWide(rec.source_rel_path, wide_src_rel) ||
        !Utf8PathToWide(rec.target_rel_path, wide_dst_rel)) {
        Log("[MenuAutoPatch] Invalid UTF-8 path in record '%s'",
            rec.target_rel_path.c_str());
        return false;
    }

    const std::wstring source_path = base_with_sep + wide_src_rel;
    const std::wstring source_bak_path = source_path + kBackupSuffix;
    staged_out.target_path = base_with_sep + wide_dst_rel;
    staged_out.backup_path = staged_out.target_path + kBackupSuffix;

    const bool source_bak_exists = RegularFileExistsWin32(source_bak_path);
    std::vector<std::uint8_t> source_file_bytes;
    bool have_valid_orig_slice = false;

    if (source_bak_exists) {
        if (!ReadEntireFileWin32(source_bak_path, kMaxPatchContainerBytes,
                                 source_file_bytes)) {
            Log("[MenuAutoPatch] Failed reading backup '%s.bak' (gle=%lu)",
                rec.source_rel_path.c_str(), ::GetLastError());
            return false;
        }
        if (rec.orig_offset > source_file_bytes.size() ||
            rec.orig_size > source_file_bytes.size() - rec.orig_offset) {
            Log("[MenuAutoPatch] Backup '%s.bak' slice out of bounds",
                rec.source_rel_path.c_str());
            return false;
        }
        have_valid_orig_slice = true;
    }

    if (!source_bak_exists &&
        ReadEntireFileWin32(source_path, kMaxPatchContainerBytes,
                            source_file_bytes) &&
        rec.orig_offset <= source_file_bytes.size() &&
        rec.orig_size <= source_file_bytes.size() - rec.orig_offset) {
        const ByteSpan candidate_slice(
            source_file_bytes.data() +
                static_cast<std::size_t>(rec.orig_offset),
            static_cast<std::size_t>(rec.orig_size));
        if (MurmurHash2(candidate_slice) == rec.orig_slice_hash) {
            have_valid_orig_slice = true;
        }
    }

    if (!have_valid_orig_slice) {
        // Recovery path when both sentinel and .bak are missing while the live
        // target on disk already matches `new_raw_hash` and `new_size`.
        if (!source_bak_exists) {
            std::vector<std::uint8_t> live_target;
            if (ReadEntireFileWin32(staged_out.target_path,
                                    kMaxAssetPayloadBytes, live_target) &&
                live_target.size() == rec.new_size &&
                MurmurHash2(ByteSpan(live_target)) == rec.new_raw_hash) {
                staged_out.needs_target_swap = false;
                staged_out.needs_backup_commit = false;
                return true;
            }
        }
        Log("[MenuAutoPatch] Source slice hash mismatch or missing file for '%s'",
            rec.source_rel_path.c_str());
        return false;
    }

    const ByteSpan orig_slice(
        source_file_bytes.data() + static_cast<std::size_t>(rec.orig_offset),
        static_cast<std::size_t>(rec.orig_size));
    const ByteSpan xor_payload = patch_blob.subspan(
        rec.payload_offset, static_cast<std::size_t>(rec.new_size));

    std::vector<std::uint8_t> new_raw;
    const UnmaskStatus unmask_rc = UnmaskRecordPayload(
        xor_payload, orig_slice, rec.orig_slice_hash, rec.new_raw_hash,
        new_raw);
    if (unmask_rc != UnmaskStatus::kOk) {
        Log("[MenuAutoPatch] Unmask failed for '%s': %s",
            rec.target_rel_path.c_str(), UnmaskStatusName(unmask_rc));
        return false;
    }

    if (!EnsureRelativeParentDirsExist(base_with_sep, wide_dst_rel)) {
        Log("[MenuAutoPatch] Failed creating parent directory for '%s' (gle=%lu)",
            rec.target_rel_path.c_str(), ::GetLastError());
        return false;
    }

    if (!StageTempFileWin32(staged_out.target_path + kStageTempSuffix,
                            ByteSpan(new_raw), staged_out.staged_target_tmp)) {
        Log("[MenuAutoPatch] Failed staging '%s.stage.tmp' (gle=%lu)",
            rec.target_rel_path.c_str(), ::GetLastError());
        return false;
    }
    staged_out.needs_target_swap = true;

    const bool target_bak_exists = RegularFileExistsWin32(staged_out.backup_path);
    const bool should_backup_target =
        !target_bak_exists &&
        ((rec.flags & kFlagBackupStandalone) != 0u ||
         source_path == staged_out.target_path);

    if (should_backup_target && RegularFileExistsWin32(staged_out.target_path)) {
        if (source_path == staged_out.target_path && !source_bak_exists) {
            if (!StageTempFileWin32(staged_out.target_path + kBackupTempSuffix,
                                    ByteSpan(source_file_bytes),
                                    staged_out.staged_backup_tmp)) {
                Log("[MenuAutoPatch] Failed staging '%s.bak.tmp' (gle=%lu)",
                    rec.target_rel_path.c_str(), ::GetLastError());
                return false;
            }
            staged_out.needs_backup_commit = true;
            return true;
        }

        std::vector<std::uint8_t> existing_target_bytes;
        if (!ReadEntireFileWin32(staged_out.target_path,
                                 kMaxPatchContainerBytes,
                                 existing_target_bytes) ||
            !StageTempFileWin32(staged_out.target_path + kBackupTempSuffix,
                                ByteSpan(existing_target_bytes),
                                staged_out.staged_backup_tmp)) {
            Log("[MenuAutoPatch] Failed staging standalone backup '%s.bak.tmp' (gle=%lu)",
                rec.target_rel_path.c_str(), ::GetLastError());
            return false;
        }
        staged_out.needs_backup_commit = true;
    }

    return true;
}

MenuPatchOutcome RunEnsureMenuAssetsPatched(const wchar_t* game_dir) {
    std::wstring base_with_sep;
    if (!NormalizeGameDirWithTrailingSep(game_dir, base_with_sep)) {
        Log("[MenuAutoPatch] Invalid or missing game directory");
        return MenuPatchOutcome::kFailed;
    }

    ScopedMutexLock mutex_lock(kMenuPatchMutexName);
    if (!mutex_lock.locked()) {
        Log("[MenuAutoPatch] Failed acquiring mutex (gle=%lu)",
            ::GetLastError());
        return MenuPatchOutcome::kFailed;
    }

    const std::wstring patch_path = base_with_sep + kMenuPatchFileName;
    std::uint64_t patch_file_size = 0u;
    const FileProbeResult probe =
        ProbeRegularFileSize(patch_path, patch_file_size);
    if (probe == FileProbeResult::kAbsent) {
        return MenuPatchOutcome::kNoPatchFile;
    }
    if (probe == FileProbeResult::kError) {
        Log("[MenuAutoPatch] Failed probing menu_patch.bin (gle=%lu)",
            ::GetLastError());
        return MenuPatchOutcome::kFailed;
    }

    const std::wstring sentinel_path =
        base_with_sep + kMenuPatchSentinelFileName;
    if (TryVerifyAlreadyAppliedFastPath(base_with_sep, patch_path,
                                        patch_file_size, sentinel_path)) {
        return MenuPatchOutcome::kAlreadyApplied;
    }

    std::vector<std::uint8_t> patch_bytes;
    if (!ReadEntireFileWin32(patch_path, kMaxPatchContainerBytes,
                             patch_bytes)) {
        Log("[MenuAutoPatch] Failed reading menu_patch.bin (gle=%lu)",
            ::GetLastError());
        return MenuPatchOutcome::kFailed;
    }

    MenuPatchManifest manifest{};
    const ManifestStatus parse_rc =
        ParseMenuPatchManifest(ByteSpan(patch_bytes), manifest);
    if (parse_rc != ManifestStatus::kOk) {
        Log("[MenuAutoPatch] Invalid menu_patch.bin: %s",
            ManifestStatusName(parse_rc));
        return MenuPatchOutcome::kFailed;
    }

    // Phase 1: Validate every record and stage all backups, targets, and the
    // applied sentinel to temporary files with FlushFileBuffers before
    // mutating any live file.
    std::vector<StagedRecord> staged_records;
    staged_records.reserve(manifest.records.size());
    AppliedSentinel sentinel{};
    sentinel.manifest_hash = manifest.header.manifest_hash;
    sentinel.patch_file_size = static_cast<std::uint64_t>(patch_bytes.size());
    sentinel.entries.reserve(manifest.records.size());

    for (const auto& rec : manifest.records) {
        StagedRecord staged{};
        if (!StageSingleRecord(base_with_sep, ByteSpan(patch_bytes), rec,
                               staged)) {
            return MenuPatchOutcome::kFailed;
        }
        staged_records.push_back(std::move(staged));
        sentinel.entries.push_back(
            AppliedSentinelEntry{rec.target_rel_path, rec.new_size});
    }

    std::vector<std::uint8_t> sentinel_bytes;
    if (SerializeAppliedSentinel(sentinel, sentinel_bytes) !=
        SentinelStatus::kOk) {
        Log("[MenuAutoPatch] Failed serializing menu_patch.applied");
        return MenuPatchOutcome::kFailed;
    }

    ScopedTempFile staged_sentinel_tmp;
    if (!StageTempFileWin32(sentinel_path + kSentinelTempSuffix,
                            ByteSpan(sentinel_bytes), staged_sentinel_tmp)) {
        Log("[MenuAutoPatch] Failed staging menu_patch.applied.tmp (gle=%lu)",
            ::GetLastError());
        return MenuPatchOutcome::kFailed;
    }

    // Phase 2: Promote immutable .bak files first, then atomically swap all
    // staged target files, and finally commit menu_patch.applied.
    for (auto& item : staged_records) {
        if (!item.needs_backup_commit) {
            continue;
        }
        if (RegularFileExistsWin32(item.backup_path)) {
            item.staged_backup_tmp.Cleanup();
            continue;
        }
        if (!item.staged_backup_tmp.CommitMove(item.backup_path, false)) {
            Log("[MenuAutoPatch] Failed committing .bak file (gle=%lu)",
                ::GetLastError());
            return MenuPatchOutcome::kFailed;
        }
    }

    for (auto& item : staged_records) {
        if (!item.needs_target_swap) {
            continue;
        }
        if (!item.staged_target_tmp.CommitMove(item.target_path, true)) {
            Log("[MenuAutoPatch] Failed swapping staged target (gle=%lu)",
                ::GetLastError());
            return MenuPatchOutcome::kFailed;
        }
    }

    if (!staged_sentinel_tmp.CommitMove(sentinel_path, true)) {
        Log("[MenuAutoPatch] Failed committing menu_patch.applied (gle=%lu)",
            ::GetLastError());
        return MenuPatchOutcome::kFailed;
    }

    Log("[MenuAutoPatch] Applied %zu menu records cleanly",
        manifest.records.size());
    return MenuPatchOutcome::kApplied;
}

}  // namespace

const char* MenuPatchOutcomeName(MenuPatchOutcome outcome) noexcept {
    switch (outcome) {
    case MenuPatchOutcome::kNoPatchFile:
        return "kNoPatchFile";
    case MenuPatchOutcome::kAlreadyApplied:
        return "kAlreadyApplied";
    case MenuPatchOutcome::kApplied:
        return "kApplied";
    case MenuPatchOutcome::kFailed:
        return "kFailed";
    }
    return "kUnknown";
}

MenuPatchOutcome EnsureMenuAssetsPatched(const wchar_t* game_dir) noexcept {
    try {
        return RunEnsureMenuAssetsPatched(game_dir);
    } catch (...) {
        Log("[MenuAutoPatch] Out-of-memory or unexpected exception during menu patch");
        return MenuPatchOutcome::kFailed;
    }
}

}  // namespace menu_autopatch
