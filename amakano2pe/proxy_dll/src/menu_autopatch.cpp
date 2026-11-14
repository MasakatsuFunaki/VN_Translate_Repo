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
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace menu_autopatch {

namespace {

using Bytes = std::vector<std::uint8_t>;
using menu_patch::AppliedSentinel;
using menu_patch::AppliedTarget;
using menu_patch::ByteView;
using menu_patch::MenuPatchHeader;
using menu_patch::MenuPatchManifest;
using menu_patch::MenuPatchRecord;
using menu_patch::ParseStatus;
using menu_patch::UnmaskStatus;

constexpr wchar_t kPatchMutexName[] = L"Local\\Amakano2PeMenuPatchMutex";
constexpr DWORD kMutexWaitTimeoutMs = 10000;

constexpr wchar_t kPatchBundleFileName[] = L"menu_patch.bin";
constexpr wchar_t kSentinelFileName[] = L"menu_patch.applied";
constexpr wchar_t kTempSuffix[] = L".tmp";
constexpr wchar_t kBackupSuffix[] = L".bak";
constexpr wchar_t kPathSeparator = L'\\';
constexpr wchar_t kDirectoryWildcard[] = L"\\*";

constexpr unsigned kSentinelMaxSizeShift = 20;
constexpr std::uint64_t kMaxSentinelBytes = 1ULL << kSentinelMaxSizeShift;
constexpr unsigned kMaxIoChunkShift = 24;
constexpr DWORD kMaxIoChunkBytes = 1u << kMaxIoChunkShift;
constexpr unsigned kBitsPerDword = 32;
constexpr std::size_t kSingleWcharStep = 1;

constexpr std::size_t kInitialModulePathChars = 260;
constexpr std::size_t kMaxModulePathChars = 32768;
constexpr std::size_t kPathGrowthFactor = 2;

constexpr double kMicrosecondsPerSecond = 1000000.0;

// Owns a Win32 kernel HANDLE whose invalid value is INVALID_HANDLE_VALUE or
// nullptr depending on the API that produced it.
class UniqueHandle {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueHandle() { Reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            Reset();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    bool Valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }
    HANDLE Get() const noexcept { return handle_; }

    void Reset() noexcept {
        if (Valid()) {
            CloseHandle(handle_);
        }
        handle_ = nullptr;
    }

private:
    HANDLE handle_ = nullptr;
};

class UniqueFindHandle {
public:
    explicit UniqueFindHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueFindHandle() {
        if (Valid()) {
            FindClose(handle_);
        }
    }

    UniqueFindHandle(const UniqueFindHandle&) = delete;
    UniqueFindHandle& operator=(const UniqueFindHandle&) = delete;

    bool Valid() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }
    HANDLE Get() const noexcept { return handle_; }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

// Holds a cross-process named mutex for the lifetime of the object.
class ScopedNamedMutex {
public:
    ScopedNamedMutex(const wchar_t* name, DWORD timeout_ms) noexcept
        : handle_(CreateMutexW(nullptr, FALSE, name)) {
        if (!handle_.Valid()) {
            return;
        }
        const DWORD rc = WaitForSingleObject(handle_.Get(), timeout_ms);
        acquired_ = (rc == WAIT_OBJECT_0 || rc == WAIT_ABANDONED);
    }
    ~ScopedNamedMutex() {
        if (acquired_) {
            ReleaseMutex(handle_.Get());
        }
    }

    ScopedNamedMutex(const ScopedNamedMutex&) = delete;
    ScopedNamedMutex& operator=(const ScopedNamedMutex&) = delete;

    bool Acquired() const noexcept { return acquired_; }

private:
    UniqueHandle handle_;
    bool acquired_ = false;
};

// Read-only memory mapping of a whole file.
class MappedFile {
public:
    MappedFile() noexcept = default;
    ~MappedFile() {
        if (view_ != nullptr) {
            UnmapViewOfFile(view_);
        }
    }

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    bool Open(const std::wstring& path, std::uint64_t expected_size) noexcept {
        if (expected_size == 0 || expected_size > SIZE_MAX) {
            return false;
        }
        file_ = UniqueHandle(CreateFileW(path.c_str(), GENERIC_READ,
                                         FILE_SHARE_READ, nullptr,
                                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                         nullptr));
        if (!file_.Valid()) {
            return false;
        }
        mapping_ = UniqueHandle(CreateFileMappingW(file_.Get(), nullptr,
                                                   PAGE_READONLY, 0, 0,
                                                   nullptr));
        if (!mapping_.Valid()) {
            return false;
        }
        view_ = MapViewOfFile(mapping_.Get(), FILE_MAP_READ, 0, 0, 0);
        if (view_ == nullptr) {
            return false;
        }
        size_ = static_cast<std::size_t>(expected_size);
        return true;
    }

    ByteView View() const noexcept {
        return ByteView{static_cast<const std::uint8_t*>(view_), size_};
    }

private:
    UniqueHandle file_;
    UniqueHandle mapping_;
    void* view_ = nullptr;
    std::size_t size_ = 0;
};

class ElapsedTimer {
public:
    ElapsedTimer() noexcept {
        QueryPerformanceFrequency(&frequency_);
        QueryPerformanceCounter(&start_);
    }

    double Microseconds() const noexcept {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        if (frequency_.QuadPart == 0) {
            return 0.0;
        }
        return static_cast<double>(now.QuadPart - start_.QuadPart) *
               kMicrosecondsPerSecond /
               static_cast<double>(frequency_.QuadPart);
    }

private:
    LARGE_INTEGER frequency_{};
    LARGE_INTEGER start_{};
};

std::optional<std::wstring> Utf8ToWide(std::string_view utf8) {
    if (utf8.empty()) {
        return std::wstring();
    }
    const int needed =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                            static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        return std::nullopt;
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
        static_cast<int>(utf8.size()), wide.data(), needed);
    if (written != needed) {
        return std::nullopt;
    }
    return wide;
}

std::string WideToUtf8(std::wstring_view wide) {
    if (wide.empty()) {
        return std::string();
    }
    const int needed =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                            nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return std::string();
    }
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        utf8.data(), needed, nullptr, nullptr);
    return utf8;
}

std::wstring UpperInvariant(std::wstring_view text) {
    if (text.empty()) {
        return std::wstring();
    }
    std::wstring upper(text.size(), L'\0');
    const int written = LCMapStringEx(
        LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, text.data(),
        static_cast<int>(text.size()), upper.data(),
        static_cast<int>(upper.size()), nullptr, nullptr, 0);
    if (written != static_cast<int>(text.size())) {
        return std::wstring(text);
    }
    return upper;
}

std::wstring JoinPath(const std::wstring& dir, std::wstring_view rel) {
    std::wstring joined = dir;
    if (!joined.empty() && joined.back() != kPathSeparator) {
        joined += kPathSeparator;
    }
    joined.append(rel.data(), rel.size());
    return joined;
}

std::optional<std::uint64_t> RegularFileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        return std::nullopt;
    }
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return std::nullopt;
    }
    return (static_cast<std::uint64_t>(data.nFileSizeHigh) << kBitsPerDword) |
           static_cast<std::uint64_t>(data.nFileSizeLow);
}

bool ReadExact(HANDLE file, std::uint8_t* dst, std::size_t size) noexcept {
    std::size_t done = 0;
    while (done < size) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(size - done, kMaxIoChunkBytes));
        DWORD got = 0;
        if (!ReadFile(file, dst + done, chunk, &got, nullptr) || got != chunk) {
            return false;
        }
        done += got;
    }
    return true;
}

bool WriteExact(HANDLE file, const std::uint8_t* src, std::size_t size) noexcept {
    std::size_t done = 0;
    while (done < size) {
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(size - done, kMaxIoChunkBytes));
        DWORD written = 0;
        if (!WriteFile(file, src + done, chunk, &written, nullptr) ||
            written != chunk) {
            return false;
        }
        done += written;
    }
    return true;
}

// Reads `[offset, offset + size)` of `path` without loading the whole file.
bool ReadFileSlice(const std::wstring& path,
                   std::uint64_t offset,
                   std::uint32_t size,
                   Bytes& out) {
    UniqueHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                  nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.Valid()) {
        return false;
    }
    LARGE_INTEGER file_size{};
    if (!GetFileSizeEx(file.Get(), &file_size) || file_size.QuadPart < 0) {
        return false;
    }
    const auto total = static_cast<std::uint64_t>(file_size.QuadPart);
    if (offset > total || size > total - offset) {
        return false;
    }
    LARGE_INTEGER seek{};
    seek.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file.Get(), seek, nullptr, FILE_BEGIN)) {
        return false;
    }
    Bytes slice(size);
    if (!ReadExact(file.Get(), slice.data(), slice.size())) {
        return false;
    }
    out = std::move(slice);
    return true;
}

bool ReadWholeFile(const std::wstring& path,
                   std::uint64_t max_bytes,
                   Bytes& out) {
    const std::optional<std::uint64_t> size = RegularFileSize(path);
    if (!size || *size == 0 || *size > max_bytes) {
        return false;
    }
    return ReadFileSlice(path, 0, static_cast<std::uint32_t>(*size), out);
}

bool ClearReadOnlyAttribute(const std::wstring& path) noexcept {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES ||
        (attrs & FILE_ATTRIBUTE_READONLY) == 0) {
        return true;
    }
    return SetFileAttributesW(path.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY) !=
           FALSE;
}

// Writes `<path>.tmp`, flushes it, and swaps it over `path`.
bool WriteFileAtomically(const std::wstring& path, ByteView bytes) {
    const std::wstring tmp_path = path + kTempSuffix;
    {
        UniqueHandle file(CreateFileW(tmp_path.c_str(), GENERIC_WRITE, 0,
                                      nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file.Valid()) {
            return false;
        }
        if (!WriteExact(file.Get(), bytes.data, bytes.size) ||
            !FlushFileBuffers(file.Get())) {
            file.Reset();
            DeleteFileW(tmp_path.c_str());
            return false;
        }
    }
    ClearReadOnlyAttribute(path);
    if (!MoveFileExW(tmp_path.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp_path.c_str());
        return false;
    }
    return true;
}

// Copies `src` to a fresh `dst` through `<dst>.tmp`; never replaces `dst`.
bool CopyFileAtomically(const std::wstring& src, const std::wstring& dst) {
    const std::wstring tmp_path = dst + kTempSuffix;
    if (!CopyFileW(src.c_str(), tmp_path.c_str(), FALSE)) {
        return false;
    }
    if (!MoveFileExW(tmp_path.c_str(), dst.c_str(), MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp_path.c_str());
        return false;
    }
    return true;
}

// Creates every missing directory between `root` and the file `rel_path`.
bool EnsureParentDirectories(const std::wstring& root,
                             std::wstring_view rel_path) {
    std::size_t sep = rel_path.find(kPathSeparator);
    while (sep != std::wstring_view::npos) {
        const std::wstring dir = JoinPath(root, rel_path.substr(0, sep));
        if (!CreateDirectoryW(dir.c_str(), nullptr) &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            return false;
        }
        sep = rel_path.find(kPathSeparator, sep + kSingleWcharStep);
    }
    return true;
}

bool FileHasDigest(const std::wstring& path,
                   std::uint32_t expected_size,
                   std::uint32_t expected_hash) {
    const std::optional<std::uint64_t> size = RegularFileSize(path);
    if (!size || *size != expected_size) {
        return false;
    }
    Bytes existing;
    if (!ReadFileSlice(path, 0, expected_size, existing)) {
        return false;
    }
    return menu_patch::MurmurHash2(existing.data(), existing.size()) ==
           expected_hash;
}

// Splits a wide relative path into (parent directory, file name).
std::pair<std::wstring, std::wstring> SplitParent(std::wstring_view rel_path) {
    const std::size_t sep = rel_path.rfind(kPathSeparator);
    if (sep == std::wstring_view::npos) {
        return {std::wstring(), std::wstring(rel_path)};
    }
    return {std::wstring(rel_path.substr(0, sep)),
            std::wstring(rel_path.substr(sep + kSingleWcharStep))};
}

// Upper-cased file name -> size for every regular file directly in `dir`.
std::map<std::wstring, std::uint64_t> ListDirectoryFileSizes(
    const std::wstring& dir) {
    std::map<std::wstring, std::uint64_t> sizes;
    WIN32_FIND_DATAW data{};
    const std::wstring pattern = dir + kDirectoryWildcard;
    UniqueFindHandle find(FindFirstFileExW(pattern.c_str(), FindExInfoBasic,
                                           &data, FindExSearchNameMatch,
                                           nullptr, FIND_FIRST_EX_LARGE_FETCH));
    if (!find.Valid()) {
        return sizes;
    }
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        const std::uint64_t size =
            (static_cast<std::uint64_t>(data.nFileSizeHigh) << kBitsPerDword) |
            static_cast<std::uint64_t>(data.nFileSizeLow);
        sizes.emplace(UpperInvariant(data.cFileName), size);
    } while (FindNextFileW(find.Get(), &data));
    return sizes;
}

// True iff every sentinel target exists under `game_dir` with its recorded
// size. Enumerates each parent directory once instead of probing each file.
bool AllTargetsPresent(const std::wstring& game_dir,
                       const std::vector<AppliedTarget>& targets) {
    std::map<std::wstring, std::vector<std::pair<std::wstring, std::uint64_t>>>
        by_parent;
    for (const AppliedTarget& target : targets) {
        const std::optional<std::wstring> wide = Utf8ToWide(target.target_rel_path);
        if (!wide) {
            return false;
        }
        auto [parent, name] = SplitParent(*wide);
        by_parent[UpperInvariant(parent)].emplace_back(UpperInvariant(name),
                                                       target.size);
    }
    for (const auto& [parent, expected_files] : by_parent) {
        const std::wstring dir =
            parent.empty() ? game_dir : JoinPath(game_dir, parent);
        const std::map<std::wstring, std::uint64_t> actual =
            ListDirectoryFileSizes(dir);
        for (const auto& [name, size] : expected_files) {
            const auto it = actual.find(name);
            if (it == actual.end() || it->second != size) {
                return false;
            }
        }
    }
    return true;
}

bool ResolveHostExecutableDirectory(std::wstring& dir_out) {
    std::size_t capacity = kInitialModulePathChars;
    while (capacity <= kMaxModulePathChars) {
        std::vector<wchar_t> buf(capacity, L'\0');
        SetLastError(ERROR_SUCCESS);
        const DWORD len =
            GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0) {
            return false;
        }
        if (len < buf.size() && GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            const std::wstring path(buf.data(), len);
            const std::size_t sep = path.find_last_of(L"\\/");
            if (sep == std::wstring::npos) {
                return false;
            }
            dir_out = path.substr(0, sep);
            return true;
        }
        capacity *= kPathGrowthFactor;
    }
    return false;
}

// One serialized auto-patch pass over a single game directory.
class MenuPatchRun {
public:
    explicit MenuPatchRun(std::wstring game_dir)
        : game_dir_(std::move(game_dir)),
          bundle_path_(JoinPath(game_dir_, kPatchBundleFileName)),
          sentinel_path_(JoinPath(game_dir_, kSentinelFileName)) {}

    PatchOutcome Execute() {
        const ElapsedTimer timer;
        const std::optional<std::uint64_t> bundle_size =
            RegularFileSize(bundle_path_);
        if (!bundle_size) {
            Log("MenuAutoPatch: INFO menu_patch.bin not present; menus stay "
                "untranslated");
            return PatchOutcome::kNoPatchBundle;
        }

        const ScopedNamedMutex lock(kPatchMutexName, kMutexWaitTimeoutMs);
        if (!lock.Acquired()) {
            Log("MenuAutoPatch: could not acquire Local\\Amakano2PeMenuPatchMutex");
            return PatchOutcome::kFailed;
        }

        MenuPatchHeader header;
        if (!ReadBundleHeader(header)) {
            return PatchOutcome::kFailed;
        }
        if (SentinelMatches(header, *bundle_size)) {
            Log("MenuAutoPatch: overrides already current (%.0f us)",
                timer.Microseconds());
            return PatchOutcome::kAlreadyApplied;
        }

        const PatchOutcome outcome = ApplyBundle(*bundle_size);
        Log("MenuAutoPatch: %s in %.0f us", PatchOutcomeName(outcome),
            timer.Microseconds());
        return outcome;
    }

private:
    bool ReadBundleHeader(MenuPatchHeader& header) const {
        Bytes bytes;
        if (!ReadFileSlice(bundle_path_, 0,
                           static_cast<std::uint32_t>(menu_patch::kFileHeaderSize),
                           bytes)) {
            Log("MenuAutoPatch: menu_patch.bin header unreadable");
            return false;
        }
        const ParseStatus status = menu_patch::ParseMenuPatchHeader(
            ByteView{bytes.data(), bytes.size()}, header);
        if (status != ParseStatus::kOk) {
            Log("MenuAutoPatch: menu_patch.bin rejected: %s",
                menu_patch::ParseStatusName(status));
            return false;
        }
        return true;
    }

    bool SentinelMatches(const MenuPatchHeader& header,
                         std::uint64_t bundle_size) const {
        Bytes bytes;
        if (!ReadWholeFile(sentinel_path_, kMaxSentinelBytes, bytes)) {
            return false;
        }
        AppliedSentinel sentinel;
        if (!menu_patch::ParseAppliedSentinel(ByteView{bytes.data(), bytes.size()},
                                              sentinel)) {
            return false;
        }
        if (sentinel.manifest_hash != header.manifest_hash ||
            sentinel.patch_file_size != bundle_size ||
            sentinel.targets.size() != header.record_count) {
            return false;
        }
        return AllTargetsPresent(game_dir_, sentinel.targets);
    }

    PatchOutcome ApplyBundle(std::uint64_t bundle_size) {
        MappedFile bundle;
        if (!bundle.Open(bundle_path_, bundle_size)) {
            Log("MenuAutoPatch: cannot map menu_patch.bin (gle=%lu)",
                GetLastError());
            return PatchOutcome::kFailed;
        }
        MenuPatchManifest manifest;
        const ParseStatus status =
            menu_patch::ParseMenuPatchContainer(bundle.View(), manifest);
        if (status != ParseStatus::kOk) {
            Log("MenuAutoPatch: menu_patch.bin rejected: %s",
                menu_patch::ParseStatusName(status));
            return PatchOutcome::kFailed;
        }

        AppliedSentinel sentinel;
        sentinel.manifest_hash = manifest.header.manifest_hash;
        sentinel.patch_file_size = bundle_size;
        std::size_t failures = 0;
        for (const MenuPatchRecord& record : manifest.records) {
            if (!ApplyRecord(bundle.View(), record)) {
                ++failures;
                continue;
            }
            sentinel.targets.push_back(
                AppliedTarget{record.target_rel_path, record.new_raw_size});
        }
        if (failures > 0) {
            Log("MenuAutoPatch: %zu of %zu records failed; will retry next launch",
                failures, manifest.records.size());
            return PatchOutcome::kPartiallyApplied;
        }

        const Bytes sentinel_bytes = menu_patch::SerializeAppliedSentinel(sentinel);
        if (!WriteFileAtomically(sentinel_path_, ByteView{sentinel_bytes.data(),
                                                          sentinel_bytes.size()})) {
            Log("MenuAutoPatch: cannot write menu_patch.applied (gle=%lu)",
                GetLastError());
            return PatchOutcome::kPartiallyApplied;
        }
        Log("MenuAutoPatch: reconstructed %zu override files",
            manifest.records.size());
        return PatchOutcome::kApplied;
    }

    bool ApplyRecord(ByteView bundle, const MenuPatchRecord& record) {
        const std::optional<std::wstring> target_rel =
            Utf8ToWide(record.target_rel_path);
        const std::optional<std::wstring> source_rel =
            Utf8ToWide(record.source_rel_path);
        if (!target_rel || !source_rel) {
            return Fail(record, "path is not valid UTF-8");
        }
        const std::wstring target_path = JoinPath(game_dir_, *target_rel);

        std::wstring slice_path = JoinPath(game_dir_, *source_rel);
        if (menu_patch::IsBackupStandalone(record)) {
            const std::optional<std::wstring> backup = EnsureBackup(record, target_path);
            if (!backup) {
                return false;
            }
            slice_path = *backup;
        }

        Bytes orig_slice;
        if (!ReadFileSlice(slice_path, record.orig_offset, record.orig_size,
                           orig_slice)) {
            return Fail(record, "original slice unreadable or out of range");
        }
        Bytes new_raw;
        const UnmaskStatus unmask = menu_patch::UnmaskRecordPayload(
            bundle, record, ByteView{orig_slice.data(), orig_slice.size()},
            new_raw);
        if (unmask != UnmaskStatus::kOk) {
            return Fail(record, menu_patch::UnmaskStatusName(unmask));
        }

        if (FileHasDigest(target_path, record.new_raw_size, record.new_raw_hash)) {
            return true;
        }
        if (!EnsureParentDirectories(game_dir_, *target_rel)) {
            return Fail(record, "cannot create parent directory");
        }
        if (!WriteFileAtomically(target_path,
                                 ByteView{new_raw.data(), new_raw.size()})) {
            return Fail(record, "atomic write failed");
        }
        return true;
    }

    // Returns the path of `<target>.bak`, creating it from the untouched
    // original only after confirming the original slice digest.
    std::optional<std::wstring> EnsureBackup(const MenuPatchRecord& record,
                                             const std::wstring& target_path) {
        const std::wstring backup_path = target_path + kBackupSuffix;
        if (RegularFileSize(backup_path)) {
            return backup_path;
        }
        Bytes slice;
        if (!ReadFileSlice(target_path, record.orig_offset, record.orig_size,
                           slice)) {
            Fail(record, "standalone original missing and no .bak exists");
            return std::nullopt;
        }
        if (menu_patch::MurmurHash2(slice.data(), slice.size()) !=
            record.orig_slice_hash) {
            Fail(record, "standalone file is not the retail original; "
                         "refusing to back it up");
            return std::nullopt;
        }
        if (!CopyFileAtomically(target_path, backup_path)) {
            Fail(record, "cannot create .bak backup");
            return std::nullopt;
        }
        return backup_path;
    }

    bool Fail(const MenuPatchRecord& record, const char* reason) const {
        Log("MenuAutoPatch: record %s -> %s failed: %s (gle=%lu)",
            record.source_rel_path.c_str(), record.target_rel_path.c_str(),
            reason, GetLastError());
        return false;
    }

    std::wstring game_dir_;
    std::wstring bundle_path_;
    std::wstring sentinel_path_;
};

}  // namespace

const char* PatchOutcomeName(PatchOutcome outcome) noexcept {
    switch (outcome) {
    case PatchOutcome::kNoPatchBundle:
        return "no patch bundle";
    case PatchOutcome::kAlreadyApplied:
        return "already applied";
    case PatchOutcome::kApplied:
        return "applied";
    case PatchOutcome::kPartiallyApplied:
        return "partially applied";
    case PatchOutcome::kFailed:
        return "failed";
    }
    return "unknown";
}

PatchOutcome EnsureMenuAssetsPatched(const std::wstring& game_dir) noexcept {
    if (game_dir.empty()) {
        return PatchOutcome::kFailed;
    }
    try {
        MenuPatchRun run(game_dir);
        return run.Execute();
    } catch (const std::exception& e) {
        Log("MenuAutoPatch: exception: %s", e.what());
        return PatchOutcome::kFailed;
    } catch (...) {
        Log("MenuAutoPatch: unknown exception");
        return PatchOutcome::kFailed;
    }
}

PatchOutcome EnsureMenuAssetsPatchedForHostProcess() noexcept {
    try {
        std::wstring game_dir;
        if (!ResolveHostExecutableDirectory(game_dir)) {
            Log("MenuAutoPatch: cannot resolve host executable directory");
            return PatchOutcome::kFailed;
        }
        Log("MenuAutoPatch: game directory %s", WideToUtf8(game_dir).c_str());
        return EnsureMenuAssetsPatched(game_dir);
    } catch (...) {
        Log("MenuAutoPatch: unknown exception");
        return PatchOutcome::kFailed;
    }
}

}  // namespace menu_autopatch
