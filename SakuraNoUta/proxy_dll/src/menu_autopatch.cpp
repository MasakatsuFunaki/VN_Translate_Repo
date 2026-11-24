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
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace menu_autopatch {
namespace {

using menu_patch_format::AppliedSentinel;
using menu_patch_format::AppliedSentinelEntry;
using menu_patch_format::ArcEntryReplacement;
using menu_patch_format::ArcStatus;
using menu_patch_format::ArcStatusName;
using menu_patch_format::ByteSpan;
using menu_patch_format::FindArc20EntrySlice;
using menu_patch_format::kFlagArchiveEntry;
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
using menu_patch_format::RepackArc20Entries;
using menu_patch_format::SentinelStatus;
using menu_patch_format::SerializeAppliedSentinel;
using menu_patch_format::UnmaskRecordPayload;
using menu_patch_format::UnmaskStatus;
using menu_patch_format::UnmaskStatusName;
using menu_patch_format::Utf8PathToWide;

constexpr DWORD kMutexWaitTimeoutMs = 15000u;
constexpr unsigned int kHighDwordShift = 32u;
constexpr std::size_t kDriveRootPathChars = 3u;
constexpr std::uint64_t kMaxPatchContainerBytes =
    512ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxSourceArchiveBytes =
    1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMaxSentinelFileBytes =
    4ULL * 1024ULL * 1024ULL;
constexpr DWORD kMaxIoChunkBytes = 64u * 1024u * 1024u;

constexpr wchar_t kStageTempSuffix[] = L".stage.tmp";
constexpr wchar_t kBackupSuffix[] = L".bak";
constexpr wchar_t kAltBackupSuffix[] = L".backup";
constexpr wchar_t kBackupTempSuffix[] = L".bak.tmp";
constexpr wchar_t kSentinelTempSuffix[] = L".tmp";

char ToAsciiLower(char ch) noexcept {
    if (ch >= 'A' && ch <= 'Z') {
        return static_cast<char>(ch - 'A' + 'a');
    }
    return ch;
}

std::string LowercaseAsciiCopy(std::string_view sv) {
    std::string lowered(sv.size(), '\0');
    for (std::size_t i = 0u; i < sv.size(); ++i) {
        lowered[i] = ToAsciiLower(sv[i]);
    }
    return lowered;
}

bool EqualsIgnoreAsciiCase(std::string_view lhs,
                           std::string_view rhs) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0u; i < lhs.size(); ++i) {
        if (ToAsciiLower(lhs[i]) != ToAsciiLower(rhs[i])) {
            return false;
        }
    }
    return true;
}

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
    while (normalized.size() > kDriveRootPathChars &&
           normalized.back() == L'\\') {
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

// Fast-path verifies menu_patch.applied ("SNUMAPL1" v1): patch_file_size,
// manifest_hash, non-empty sentinel.entries (1 entry per unique target file,
// e.g. 1 "sysgrp.arc" entry for 83 kFlagArchiveEntry records), and exact
// on-disk byte size for every unique target file.
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
    if (sentinel.patch_file_size != actual_patch_size ||
        sentinel.entries.empty()) {
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
        sentinel.entries.size() > header.record_count) {
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

enum class CachedBlobState {
    kUnprobed,
    kAbsent,
    kLoaded,
    kReadError,
};

struct CachedCandidateBlob {
    CachedBlobState state = CachedBlobState::kUnprobed;
    std::vector<std::uint8_t> bytes;
};

struct SourceCandidateGroup {
    std::wstring base_path;
    CachedCandidateBlob bak;
    CachedCandidateBlob backup;
    CachedCandidateBlob live;
};

// Caches loaded source files (.bak, .backup, and live) in memory keyed by
// lowercase source_rel_path so multi-record archives (sysgrp.arc, data02501.arc,
// data02502.arc) are read from disk at most once per run.
class SourceFileCache {
public:
    explicit SourceFileCache(const std::wstring& base_with_sep)
        : base_with_sep_(base_with_sep) {}

    SourceCandidateGroup* GetOrCreateGroup(std::string_view source_rel_path) {
        const std::string key = LowercaseAsciiCopy(source_rel_path);
        auto it = groups_.find(key);
        if (it != groups_.end()) {
            return &it->second;
        }
        std::wstring wide_rel;
        if (!Utf8PathToWide(source_rel_path, wide_rel)) {
            return nullptr;
        }
        SourceCandidateGroup created{};
        created.base_path = base_with_sep_ + wide_rel;
        auto [inserted_it, _] = groups_.emplace(key, std::move(created));
        return &inserted_it->second;
    }

    static CachedBlobState EnsureLoaded(const std::wstring& path,
                                        CachedCandidateBlob& slot) {
        if (slot.state != CachedBlobState::kUnprobed) {
            return slot.state;
        }
        std::uint64_t file_size = 0u;
        const FileProbeResult probe = ProbeRegularFileSize(path, file_size);
        if (probe == FileProbeResult::kAbsent) {
            slot.state = CachedBlobState::kAbsent;
            return slot.state;
        }
        if (probe == FileProbeResult::kError) {
            slot.state = CachedBlobState::kReadError;
            return slot.state;
        }
        if (!ReadEntireFileWin32(path, kMaxSourceArchiveBytes, slot.bytes)) {
            slot.state = CachedBlobState::kReadError;
            return slot.state;
        }
        slot.state = CachedBlobState::kLoaded;
        return slot.state;
    }

    static CachedBlobState EnsureBakLoaded(SourceCandidateGroup& grp) {
        return EnsureLoaded(grp.base_path + kBackupSuffix, grp.bak);
    }

    static CachedBlobState EnsureBackupLoaded(SourceCandidateGroup& grp) {
        return EnsureLoaded(grp.base_path + kAltBackupSuffix, grp.backup);
    }

    static CachedBlobState EnsureLiveLoaded(SourceCandidateGroup& grp) {
        return EnsureLoaded(grp.base_path, grp.live);
    }

private:
    const std::wstring& base_with_sep_;
    std::unordered_map<std::string, SourceCandidateGroup> groups_;
};

// Resolves `rec`'s original byte slice inside a single candidate buffer:
//   1. Checks `rec.orig_offset` + `rec.orig_size` against `rec.orig_slice_hash`.
//   2. If that misses and `!rec.entry_name.empty()`, queries
//      FindArc20EntrySlice(buf, rec.entry_name) and checks `orig_slice_hash`
//      so repacked archives with shifted entry offsets still resolve cleanly.
bool TryResolveRecordSliceInBuffer(ByteSpan buf,
                                   const MenuPatchRecord& rec,
                                   ByteSpan& slice_out) noexcept {
    if (rec.orig_offset <= buf.size() &&
        rec.orig_size <= buf.size() - rec.orig_offset) {
        const ByteSpan direct = buf.subspan(
            static_cast<std::size_t>(rec.orig_offset),
            static_cast<std::size_t>(rec.orig_size));
        if (MurmurHash2(direct) == rec.orig_slice_hash) {
            slice_out = direct;
            return true;
        }
    }

    if (rec.entry_name.empty()) {
        return false;
    }

    std::uint64_t entry_offset = 0u;
    std::uint64_t entry_size = 0u;
    if (FindArc20EntrySlice(buf, rec.entry_name, entry_offset, entry_size) !=
        ArcStatus::kOk) {
        return false;
    }
    if (entry_size != rec.orig_size || entry_offset > buf.size() ||
        entry_size > buf.size() - entry_offset) {
        return false;
    }

    const ByteSpan by_name = buf.subspan(
        static_cast<std::size_t>(entry_offset),
        static_cast<std::size_t>(entry_size));
    if (MurmurHash2(by_name) != rec.orig_slice_hash) {
        return false;
    }
    slice_out = by_name;
    return true;
}

bool AllGroupRecordsMatchBuffer(
    ByteSpan buf,
    const std::vector<const MenuPatchRecord*>& group_records,
    std::vector<ByteSpan>& slices_out) {
    slices_out.clear();
    slices_out.reserve(group_records.size());
    for (const MenuPatchRecord* rec : group_records) {
        ByteSpan slice{};
        if (!TryResolveRecordSliceInBuffer(buf, *rec, slice)) {
            slices_out.clear();
            return false;
        }
        slices_out.push_back(slice);
    }
    return true;
}

bool AllArchiveEntriesAlreadyPatchedInLive(
    ByteSpan live_arc,
    const std::vector<const MenuPatchRecord*>& group_records) noexcept {
    for (const MenuPatchRecord* rec : group_records) {
        std::uint64_t entry_offset = 0u;
        std::uint64_t entry_size = 0u;
        if (FindArc20EntrySlice(live_arc, rec->entry_name, entry_offset,
                                entry_size) != ArcStatus::kOk) {
            return false;
        }
        if (entry_size != rec->new_size || entry_offset > live_arc.size() ||
            entry_size > live_arc.size() - entry_offset) {
            return false;
        }
        const ByteSpan slice = live_arc.subspan(
            static_cast<std::size_t>(entry_offset),
            static_cast<std::size_t>(entry_size));
        if (MurmurHash2(slice) != rec->new_raw_hash) {
            return false;
        }
    }
    return true;
}

struct StagedTarget {
    std::wstring target_path;
    std::wstring backup_path;
    ScopedTempFile staged_target_tmp;
    ScopedTempFile staged_backup_tmp;
    bool needs_target_swap = false;
    bool needs_backup_commit = false;
};

bool StageStandaloneRecord(const std::wstring& base_with_sep,
                           ByteSpan patch_blob,
                           const MenuPatchRecord& rec,
                           SourceFileCache& cache,
                           StagedTarget& staged_out,
                           std::uint64_t& expected_size_out) {
    std::wstring wide_dst_rel;
    if (!Utf8PathToWide(rec.target_rel_path, wide_dst_rel)) {
        Log("[MenuAutoPatch] Invalid UTF-8 target path '%s'",
            rec.target_rel_path.c_str());
        return false;
    }

    SourceCandidateGroup* src_group = cache.GetOrCreateGroup(rec.source_rel_path);
    if (src_group == nullptr) {
        Log("[MenuAutoPatch] Invalid UTF-8 source path '%s'",
            rec.source_rel_path.c_str());
        return false;
    }

    staged_out.target_path = base_with_sep + wide_dst_rel;
    staged_out.backup_path = staged_out.target_path + kBackupSuffix;

    ByteSpan orig_slice{};
    const std::vector<std::uint8_t>* matched_source_bytes = nullptr;
    bool have_orig_slice = false;

    const CachedBlobState bak_state = SourceFileCache::EnsureBakLoaded(*src_group);
    if (bak_state == CachedBlobState::kReadError) {
        Log("[MenuAutoPatch] Failed reading '%s.bak' (gle=%lu)",
            rec.source_rel_path.c_str(), ::GetLastError());
        return false;
    }
    if (bak_state == CachedBlobState::kLoaded &&
        TryResolveRecordSliceInBuffer(ByteSpan(src_group->bak.bytes), rec,
                                      orig_slice)) {
        matched_source_bytes = &src_group->bak.bytes;
        have_orig_slice = true;
    }

    if (!have_orig_slice) {
        const CachedBlobState backup_state =
            SourceFileCache::EnsureBackupLoaded(*src_group);
        if (backup_state == CachedBlobState::kReadError) {
            Log("[MenuAutoPatch] Failed reading '%s.backup' (gle=%lu)",
                rec.source_rel_path.c_str(), ::GetLastError());
            return false;
        }
        if (backup_state == CachedBlobState::kLoaded &&
            TryResolveRecordSliceInBuffer(ByteSpan(src_group->backup.bytes),
                                          rec, orig_slice)) {
            matched_source_bytes = &src_group->backup.bytes;
            have_orig_slice = true;
        }
    }

    if (!have_orig_slice) {
        const CachedBlobState live_state =
            SourceFileCache::EnsureLiveLoaded(*src_group);
        if (live_state == CachedBlobState::kReadError) {
            Log("[MenuAutoPatch] Failed reading '%s' (gle=%lu)",
                rec.source_rel_path.c_str(), ::GetLastError());
            return false;
        }
        if (live_state == CachedBlobState::kLoaded &&
            TryResolveRecordSliceInBuffer(ByteSpan(src_group->live.bytes), rec,
                                          orig_slice)) {
            matched_source_bytes = &src_group->live.bytes;
            have_orig_slice = true;
        }
    }

    if (!have_orig_slice) {
        if (bak_state == CachedBlobState::kAbsent &&
            !RegularFileExistsWin32(staged_out.backup_path)) {
            std::vector<std::uint8_t> live_target;
            if (ReadEntireFileWin32(staged_out.target_path,
                                    kMaxAssetPayloadBytes, live_target) &&
                live_target.size() == rec.new_size &&
                MurmurHash2(ByteSpan(live_target)) == rec.new_raw_hash) {
                staged_out.needs_target_swap = false;
                staged_out.needs_backup_commit = false;
                expected_size_out = rec.new_size;
                return true;
            }
        }
        Log("[MenuAutoPatch] Source slice hash mismatch or missing source for '%s' -> '%s'",
            rec.source_rel_path.c_str(), rec.target_rel_path.c_str());
        return false;
    }

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
    expected_size_out = rec.new_size;

    const bool same_src_and_dst =
        EqualsIgnoreAsciiCase(rec.source_rel_path, rec.target_rel_path);
    const bool target_bak_exists = RegularFileExistsWin32(staged_out.backup_path);
    const bool wants_backup =
        !target_bak_exists &&
        ((rec.flags & kFlagBackupStandalone) != 0u || same_src_and_dst);

    if (!wants_backup || !RegularFileExistsWin32(staged_out.target_path)) {
        return true;
    }

    if (same_src_and_dst && matched_source_bytes != nullptr) {
        if (!StageTempFileWin32(staged_out.target_path + kBackupTempSuffix,
                                ByteSpan(*matched_source_bytes),
                                staged_out.staged_backup_tmp)) {
            Log("[MenuAutoPatch] Failed staging '%s.bak.tmp' (gle=%lu)",
                rec.target_rel_path.c_str(), ::GetLastError());
            return false;
        }
        staged_out.needs_backup_commit = true;
        return true;
    }

    std::vector<std::uint8_t> existing_target_bytes;
    if (!ReadEntireFileWin32(staged_out.target_path, kMaxSourceArchiveBytes,
                             existing_target_bytes)) {
        Log("[MenuAutoPatch] Failed reading existing '%s' for backup (gle=%lu)",
            rec.target_rel_path.c_str(), ::GetLastError());
        return false;
    }

    // Do not overwrite/create .bak from a target that already holds the
    // patched output (e.g. when menu_patch.applied was deleted after Launch 1).
    if (existing_target_bytes.size() == rec.new_size &&
        MurmurHash2(ByteSpan(existing_target_bytes)) == rec.new_raw_hash) {
        return true;
    }

    if (!StageTempFileWin32(staged_out.target_path + kBackupTempSuffix,
                            ByteSpan(existing_target_bytes),
                            staged_out.staged_backup_tmp)) {
        Log("[MenuAutoPatch] Failed staging standalone backup '%s.bak.tmp' (gle=%lu)",
            rec.target_rel_path.c_str(), ::GetLastError());
        return false;
    }
    staged_out.needs_backup_commit = true;
    return true;
}

bool StageArchiveGroup(
    const std::wstring& base_with_sep,
    ByteSpan patch_blob,
    std::string_view archive_rel_path,
    const std::vector<const MenuPatchRecord*>& group_records,
    SourceFileCache& cache,
    StagedTarget& staged_out,
    std::uint64_t& expected_size_out) {
    std::wstring wide_dst_rel;
    if (!Utf8PathToWide(archive_rel_path, wide_dst_rel)) {
        Log("[MenuAutoPatch] Invalid UTF-8 archive path '%.*s'",
            static_cast<int>(archive_rel_path.size()),
            archive_rel_path.data());
        return false;
    }

    SourceCandidateGroup* src_group = cache.GetOrCreateGroup(archive_rel_path);
    if (src_group == nullptr) {
        return false;
    }

    staged_out.target_path = base_with_sep + wide_dst_rel;
    staged_out.backup_path = staged_out.target_path + kBackupSuffix;

    const std::vector<std::uint8_t>* base_arc_bytes = nullptr;
    std::vector<ByteSpan> resolved_slices;

    const CachedBlobState bak_state = SourceFileCache::EnsureBakLoaded(*src_group);
    if (bak_state == CachedBlobState::kReadError) {
        Log("[MenuAutoPatch] Failed reading '%.*s.bak' (gle=%lu)",
            static_cast<int>(archive_rel_path.size()), archive_rel_path.data(),
            ::GetLastError());
        return false;
    }
    if (bak_state == CachedBlobState::kLoaded &&
        AllGroupRecordsMatchBuffer(ByteSpan(src_group->bak.bytes),
                                   group_records, resolved_slices)) {
        base_arc_bytes = &src_group->bak.bytes;
    }

    if (base_arc_bytes == nullptr) {
        const CachedBlobState backup_state =
            SourceFileCache::EnsureBackupLoaded(*src_group);
        if (backup_state == CachedBlobState::kReadError) {
            Log("[MenuAutoPatch] Failed reading '%.*s.backup' (gle=%lu)",
                static_cast<int>(archive_rel_path.size()),
                archive_rel_path.data(), ::GetLastError());
            return false;
        }
        if (backup_state == CachedBlobState::kLoaded &&
            AllGroupRecordsMatchBuffer(ByteSpan(src_group->backup.bytes),
                                       group_records, resolved_slices)) {
            base_arc_bytes = &src_group->backup.bytes;
        }
    }

    if (base_arc_bytes == nullptr) {
        const CachedBlobState live_state =
            SourceFileCache::EnsureLiveLoaded(*src_group);
        if (live_state == CachedBlobState::kReadError) {
            Log("[MenuAutoPatch] Failed reading '%.*s' (gle=%lu)",
                static_cast<int>(archive_rel_path.size()),
                archive_rel_path.data(), ::GetLastError());
            return false;
        }
        if (live_state == CachedBlobState::kLoaded &&
            AllGroupRecordsMatchBuffer(ByteSpan(src_group->live.bytes),
                                       group_records, resolved_slices)) {
            base_arc_bytes = &src_group->live.bytes;
        }
    }

    if (base_arc_bytes == nullptr) {
        // Recovery path when both .bak and sentinel are missing: if live
        // archive already has every entry_name slice matching new_raw_hash and
        // new_size, record live archive size and skip rewrite.
        if (bak_state == CachedBlobState::kAbsent &&
            SourceFileCache::EnsureLiveLoaded(*src_group) ==
                CachedBlobState::kLoaded &&
            !src_group->live.bytes.empty() &&
            src_group->live.bytes.size() <= kMaxAssetPayloadBytes &&
            AllArchiveEntriesAlreadyPatchedInLive(
                ByteSpan(src_group->live.bytes), group_records)) {
            staged_out.needs_target_swap = false;
            staged_out.needs_backup_commit = false;
            expected_size_out =
                static_cast<std::uint64_t>(src_group->live.bytes.size());
            return true;
        }
        Log("[MenuAutoPatch] Failed resolving unpatched base archive for '%.*s'",
            static_cast<int>(archive_rel_path.size()),
            archive_rel_path.data());
        return false;
    }

    std::vector<std::vector<std::uint8_t>> unmasked_payloads(
        group_records.size());
    std::vector<ArcEntryReplacement> replacements;
    replacements.reserve(group_records.size());

    for (std::size_t i = 0u; i < group_records.size(); ++i) {
        const MenuPatchRecord& rec = *group_records[i];
        const ByteSpan xor_payload = patch_blob.subspan(
            rec.payload_offset, static_cast<std::size_t>(rec.new_size));
        const UnmaskStatus unmask_rc = UnmaskRecordPayload(
            xor_payload, resolved_slices[i], rec.orig_slice_hash,
            rec.new_raw_hash, unmasked_payloads[i]);
        if (unmask_rc != UnmaskStatus::kOk) {
            Log("[MenuAutoPatch] Unmask failed for '%s::%s': %s",
                rec.target_rel_path.c_str(), rec.entry_name.c_str(),
                UnmaskStatusName(unmask_rc));
            return false;
        }
        replacements.push_back(ArcEntryReplacement{
            rec.entry_name, ByteSpan(unmasked_payloads[i])});
    }

    std::vector<std::uint8_t> repacked_arc;
    const ArcStatus repack_rc = RepackArc20Entries(
        ByteSpan(*base_arc_bytes), replacements, repacked_arc);
    if (repack_rc != ArcStatus::kOk || repacked_arc.empty() ||
        repacked_arc.size() > kMaxAssetPayloadBytes) {
        Log("[MenuAutoPatch] RepackArc20Entries failed for '%.*s': %s",
            static_cast<int>(archive_rel_path.size()),
            archive_rel_path.data(), ArcStatusName(repack_rc));
        return false;
    }

    if (!EnsureRelativeParentDirsExist(base_with_sep, wide_dst_rel)) {
        Log("[MenuAutoPatch] Failed creating parent directory for '%.*s' (gle=%lu)",
            static_cast<int>(archive_rel_path.size()),
            archive_rel_path.data(), ::GetLastError());
        return false;
    }

    if (!StageTempFileWin32(staged_out.target_path + kStageTempSuffix,
                            ByteSpan(repacked_arc),
                            staged_out.staged_target_tmp)) {
        Log("[MenuAutoPatch] Failed staging '%.*s.stage.tmp' (gle=%lu)",
            static_cast<int>(archive_rel_path.size()),
            archive_rel_path.data(), ::GetLastError());
        return false;
    }
    staged_out.needs_target_swap = true;
    expected_size_out = static_cast<std::uint64_t>(repacked_arc.size());

    if (!RegularFileExistsWin32(staged_out.backup_path)) {
        if (!StageTempFileWin32(staged_out.target_path + kBackupTempSuffix,
                                ByteSpan(*base_arc_bytes),
                                staged_out.staged_backup_tmp)) {
            Log("[MenuAutoPatch] Failed staging '%.*s.bak.tmp' (gle=%lu)",
                static_cast<int>(archive_rel_path.size()),
                archive_rel_path.data(), ::GetLastError());
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

    std::unordered_map<std::string, std::vector<const MenuPatchRecord*>>
        archive_groups;
    for (const auto& rec : manifest.records) {
        if ((rec.flags & kFlagArchiveEntry) != 0u) {
            archive_groups[LowercaseAsciiCopy(rec.target_rel_path)].push_back(
                &rec);
        }
    }

    // Phase 1: Validate every record and stage all backups, targets, and the
    // applied sentinel to temporary files with FlushFileBuffers before
    // mutating any live file.
    SourceFileCache source_cache(base_with_sep);
    std::vector<StagedTarget> staged_targets;
    staged_targets.reserve(manifest.records.size());
    std::unordered_set<std::string> staged_archive_keys;

    AppliedSentinel sentinel{};
    sentinel.manifest_hash = manifest.header.manifest_hash;
    sentinel.patch_file_size = static_cast<std::uint64_t>(patch_bytes.size());
    sentinel.entries.reserve(manifest.records.size());

    for (const auto& rec : manifest.records) {
        if ((rec.flags & kFlagArchiveEntry) != 0u) {
            const std::string key = LowercaseAsciiCopy(rec.target_rel_path);
            if (!staged_archive_keys.insert(key).second) {
                continue;
            }
            StagedTarget staged{};
            std::uint64_t expected_size = 0u;
            if (!StageArchiveGroup(base_with_sep, ByteSpan(patch_bytes),
                                   rec.target_rel_path, archive_groups[key],
                                   source_cache, staged, expected_size)) {
                return MenuPatchOutcome::kFailed;
            }
            staged_targets.push_back(std::move(staged));
            sentinel.entries.push_back(
                AppliedSentinelEntry{rec.target_rel_path, expected_size});
            continue;
        }

        StagedTarget staged{};
        std::uint64_t expected_size = 0u;
        if (!StageStandaloneRecord(base_with_sep, ByteSpan(patch_bytes), rec,
                                   source_cache, staged, expected_size)) {
            return MenuPatchOutcome::kFailed;
        }
        staged_targets.push_back(std::move(staged));
        sentinel.entries.push_back(
            AppliedSentinelEntry{rec.target_rel_path, expected_size});
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
    for (auto& item : staged_targets) {
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

    for (auto& item : staged_targets) {
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

    Log("[MenuAutoPatch] Applied %zu menu records across %zu target file(s)",
        manifest.records.size(), sentinel.entries.size());
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
