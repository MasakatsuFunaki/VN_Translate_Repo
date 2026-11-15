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

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace menu_autopatch {

namespace {

using Bytes = std::vector<std::uint8_t>;
namespace fs = std::filesystem;

constexpr std::size_t kSentinelMagicSize = 8u;
constexpr char kSentinelMagic[kSentinelMagicSize] = {
    'C', 'C', 'M', 'P', 'A', 'P', 'L', 'D'};
constexpr std::uint32_t kSentinelVersion = 1u;
constexpr std::size_t kSentinelSize = 32u;

class ScopedNamedMutex {
public:
    ScopedNamedMutex(const wchar_t* name, DWORD timeout_ms) noexcept
        : handle_(CreateMutexW(nullptr, FALSE, name)) {
        if (handle_ != nullptr) {
            const DWORD rc = WaitForSingleObject(handle_, timeout_ms);
            acquired_ = (rc == WAIT_OBJECT_0 || rc == WAIT_ABANDONED);
        }
    }

    ~ScopedNamedMutex() {
        if (acquired_ && handle_ != nullptr) {
            ReleaseMutex(handle_);
        }
        if (handle_ != nullptr) {
            CloseHandle(handle_);
        }
    }

    ScopedNamedMutex(const ScopedNamedMutex&) = delete;
    ScopedNamedMutex& operator=(const ScopedNamedMutex&) = delete;

    bool acquired() const noexcept { return acquired_; }

private:
    HANDLE handle_ = nullptr;
    bool acquired_ = false;
};

class ScopedFileHandle {
public:
    explicit ScopedFileHandle(HANDLE h) noexcept : handle_(h) {}

    ~ScopedFileHandle() {
        if (valid()) {
            CloseHandle(handle_);
        }
    }

    ScopedFileHandle(const ScopedFileHandle&) = delete;
    ScopedFileHandle& operator=(const ScopedFileHandle&) = delete;

    bool valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }
    HANDLE get() const noexcept { return handle_; }
    void reset() noexcept {
        if (valid()) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

std::uint32_t ReadLe32(const std::uint8_t* p) noexcept {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t ReadLe64(const std::uint8_t* p) noexcept {
    return static_cast<std::uint64_t>(ReadLe32(p)) |
           (static_cast<std::uint64_t>(ReadLe32(p + 4)) << 32);
}

void WriteLe32(std::uint8_t* p, std::uint32_t v) noexcept {
    p[0] = static_cast<std::uint8_t>(v & 0xFFu);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFu);
    p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFFu);
    p[3] = static_cast<std::uint8_t>((v >> 24) & 0xFFu);
}

void WriteLe64(std::uint8_t* p, std::uint64_t v) noexcept {
    WriteLe32(p, static_cast<std::uint32_t>(v & 0xFFFFFFFFu));
    WriteLe32(p + 4, static_cast<std::uint32_t>((v >> 32) & 0xFFFFFFFFu));
}

bool GetExactFileSize(const std::wstring& path, std::uint64_t& out_size) noexcept {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) ||
        (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return false;
    }
    out_size = (static_cast<std::uint64_t>(fad.nFileSizeHigh) << 32) |
               static_cast<std::uint64_t>(fad.nFileSizeLow);
    return true;
}

bool ReadFilePrefix(const std::wstring& path,
                    std::uint8_t* dst,
                    DWORD byte_count) noexcept {
    ScopedFileHandle file(CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.valid()) {
        return false;
    }
    DWORD got = 0;
    return ReadFile(file.get(), dst, byte_count, &got, nullptr) &&
           got == byte_count;
}

bool ReadEntireFile(const std::wstring& path, Bytes& out) {
    out.clear();
    ScopedFileHandle file(CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file.valid()) {
        return false;
    }
    LARGE_INTEGER li{};
    if (!GetFileSizeEx(file.get(), &li) || li.QuadPart < 0 ||
        static_cast<std::uint64_t>(li.QuadPart) >
            static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return false;
    }
    out.resize(static_cast<std::size_t>(li.QuadPart));
    std::size_t done = 0;
    while (done < out.size()) {
        const std::size_t rem = out.size() - done;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(rem, 1u << 24));
        DWORD got = 0;
        if (!ReadFile(file.get(), out.data() + done, chunk, &got, nullptr) ||
            got == 0u) {
            out.clear();
            return false;
        }
        done += got;
    }
    return true;
}

bool WriteAndCommitAtomically(const std::wstring& target_path,
                              const std::uint8_t* data,
                              std::size_t size) noexcept {
    const std::wstring tmp_path = target_path + L".tmp";
    DeleteFileW(tmp_path.c_str());

    {
        ScopedFileHandle out(CreateFileW(
            tmp_path.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!out.valid()) {
            return false;
        }
        std::size_t written_total = 0;
        while (written_total < size) {
            const std::size_t rem = size - written_total;
            const DWORD chunk =
                static_cast<DWORD>(std::min<std::size_t>(rem, 1u << 24));
            DWORD put = 0;
            if (!WriteFile(out.get(), data + written_total, chunk, &put, nullptr) ||
                put != chunk) {
                out.reset();
                DeleteFileW(tmp_path.c_str());
                return false;
            }
            written_total += put;
        }
        if (!FlushFileBuffers(out.get())) {
            out.reset();
            DeleteFileW(tmp_path.c_str());
            return false;
        }
    }

    if (!MoveFileExW(tmp_path.c_str(), target_path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp_path.c_str());
        return false;
    }
    return true;
}

bool CreateAtomicBackup(const std::wstring& src_path,
                        const std::wstring& bak_path) noexcept {
    const std::wstring bak_tmp = bak_path + L".tmp";
    DeleteFileW(bak_tmp.c_str());
    if (!CopyFileW(src_path.c_str(), bak_tmp.c_str(), FALSE)) {
        return false;
    }
    {
        ScopedFileHandle h(CreateFileW(
            bak_tmp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (h.valid()) {
            FlushFileBuffers(h.get());
        }
    }
    if (!MoveFileExW(bak_tmp.c_str(), bak_path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(bak_tmp.c_str());
        return false;
    }
    return true;
}

bool IsSentinelMatching(const std::wstring& sentinel_path,
                        std::uint32_t expected_payload_hash,
                        std::uint64_t expected_patch_file_size,
                        std::uint64_t expected_patched_cpk_size) noexcept {
    std::uint64_t sent_size = 0;
    if (!GetExactFileSize(sentinel_path, sent_size) ||
        sent_size != kSentinelSize) {
        return false;
    }
    std::array<std::uint8_t, kSentinelSize> buf{};
    if (!ReadFilePrefix(sentinel_path, buf.data(),
                        static_cast<DWORD>(kSentinelSize))) {
        return false;
    }
    if (std::memcmp(buf.data(), kSentinelMagic, kSentinelMagicSize) != 0 ||
        ReadLe32(buf.data() + 8u) != kSentinelVersion) {
        return false;
    }
    const std::uint32_t stored_hash = ReadLe32(buf.data() + 12u);
    const std::uint64_t stored_patch_sz = ReadLe64(buf.data() + 16u);
    const std::uint64_t stored_cpk_sz = ReadLe64(buf.data() + 24u);
    return stored_hash == expected_payload_hash &&
           stored_patch_sz == expected_patch_file_size &&
           stored_cpk_sz == expected_patched_cpk_size;
}

bool WriteSentinelAtomically(const std::wstring& sentinel_path,
                             std::uint32_t payload_hash,
                             std::uint64_t patch_file_size,
                             std::uint64_t patched_cpk_size) noexcept {
    std::array<std::uint8_t, kSentinelSize> buf{};
    std::memcpy(buf.data(), kSentinelMagic, kSentinelMagicSize);
    WriteLe32(buf.data() + 8u, kSentinelVersion);
    WriteLe32(buf.data() + 12u, payload_hash);
    WriteLe64(buf.data() + 16u, patch_file_size);
    WriteLe64(buf.data() + 24u, patched_cpk_size);
    return WriteAndCommitAtomically(sentinel_path, buf.data(), buf.size());
}

fs::path ParentDirOfModule(HMODULE mod) {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD len = GetModuleFileNameW(
            mod, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0u) {
            return {};
        }
        if (len < buf.size() - 1u) {
            buf.resize(len);
            return fs::path(buf).parent_path();
        }
        buf.resize(buf.size() * 2u);
    }
}

}  // namespace

struct MenuAutoPatcher::Impl {
    std::wstring mutex_name;
    std::uint32_t mutex_timeout_ms = kDefaultMutexTimeoutMs;
};

MenuAutoPatcher::MenuAutoPatcher(std::wstring mutex_name,
                                 std::uint32_t mutex_timeout_ms)
    : impl_(std::make_unique<Impl>()) {
    impl_->mutex_name = std::move(mutex_name);
    impl_->mutex_timeout_ms = mutex_timeout_ms;
}

MenuAutoPatcher::~MenuAutoPatcher() = default;
MenuAutoPatcher::MenuAutoPatcher(MenuAutoPatcher&&) noexcept = default;
MenuAutoPatcher& MenuAutoPatcher::operator=(MenuAutoPatcher&&) noexcept = default;

PatchResult MenuAutoPatcher::Apply(const fs::path& game_dir) const {
    if (game_dir.empty() || !impl_) {
        return {PatchStatus::kFailed, 0u};
    }

    ScopedNamedMutex lock(impl_->mutex_name.c_str(), impl_->mutex_timeout_ms);
    if (!lock.acquired()) {
        Log("MenuAutoPatch: failed to acquire mutex");
        return {PatchStatus::kFailed, 0u};
    }

    const std::wstring patch_path = (game_dir / L"menu_patch.bin").wstring();
    const std::wstring cpk_path = (game_dir / L"data" / L"sys.cpk").wstring();
    const std::wstring bak_path = cpk_path + L".bak";
    const std::wstring sentinel_path = cpk_path + L".applied";

    std::uint64_t patch_file_size = 0;
    if (!GetExactFileSize(patch_path, patch_file_size)) {
        return {PatchStatus::kSkippedMissingInput, 0u};
    }

    std::uint64_t cpk_size = 0;
    const bool has_cpk = GetExactFileSize(cpk_path, cpk_size);
    std::uint64_t bak_size = 0;
    bool has_bak = GetExactFileSize(bak_path, bak_size);

    if (!has_cpk && !has_bak) {
        return {PatchStatus::kSkippedMissingInput, 0u};
    }

    if (patch_file_size < menu_patch::kPatchHeaderSize) {
        Log("MenuAutoPatch: menu_patch.bin truncated (%llu bytes)",
            static_cast<unsigned long long>(patch_file_size));
        return {PatchStatus::kFailed, 0u};
    }

    std::array<std::uint8_t, menu_patch::kPatchHeaderSize> hdr_bytes{};
    if (!ReadFilePrefix(patch_path, hdr_bytes.data(),
                        static_cast<DWORD>(hdr_bytes.size()))) {
        Log("MenuAutoPatch: failed to read menu_patch.bin header");
        return {PatchStatus::kFailed, 0u};
    }

    menu_patch::PatchHeaderInfo hdr{};
    if (!menu_patch::MenuPatchPackage::ParseHeaderPrefix(
            hdr_bytes.data(), hdr_bytes.size(), hdr)) {
        Log("MenuAutoPatch: invalid CCMP header in menu_patch.bin");
        return {PatchStatus::kFailed, 0u};
    }

    // Launch 2+ <1 ms fast-path: verified backup, verified patched sys.cpk size,
    // and matching data\sys.cpk.applied sentinel.
    if (has_bak && bak_size == hdr.expected_orig_cpk_size &&
        has_cpk && cpk_size == hdr.expected_patched_cpk_size &&
        IsSentinelMatching(sentinel_path,
                           hdr.patch_payload_hash,
                           patch_file_size,
                           hdr.expected_patched_cpk_size)) {
        return {PatchStatus::kAlreadyUpToDate, cpk_size};
    }

    // Ensure pristine unpatched backup exists at data\sys.cpk.bak.
    if (!has_bak) {
        if (!has_cpk || cpk_size != hdr.expected_orig_cpk_size) {
            Log("MenuAutoPatch: unpatched sys.cpk size mismatch (%llu != %llu)",
                static_cast<unsigned long long>(cpk_size),
                static_cast<unsigned long long>(hdr.expected_orig_cpk_size));
            return {PatchStatus::kFailed, 0u};
        }
        if (!CreateAtomicBackup(cpk_path, bak_path)) {
            Log("MenuAutoPatch: failed to create sys.cpk.bak (gle=%lu)",
                GetLastError());
            return {PatchStatus::kFailed, 0u};
        }
        has_bak = GetExactFileSize(bak_path, bak_size);
    }

    if (!has_bak || bak_size != hdr.expected_orig_cpk_size) {
        Log("MenuAutoPatch: sys.cpk.bak size mismatch (%llu != %llu)",
            static_cast<unsigned long long>(bak_size),
            static_cast<unsigned long long>(hdr.expected_orig_cpk_size));
        return {PatchStatus::kFailed, 0u};
    }

    Bytes orig_cpk;
    Bytes patch_blob;
    if (!ReadEntireFile(bak_path, orig_cpk) ||
        !ReadEntireFile(patch_path, patch_blob)) {
        Log("MenuAutoPatch: failed to read sys.cpk.bak or menu_patch.bin");
        return {PatchStatus::kFailed, 0u};
    }

    Bytes patched_cpk;
    if (!menu_patch::ApplyMenuPatchInMemory(orig_cpk, patch_blob, patched_cpk)) {
        Log("MenuAutoPatch: ApplyMenuPatchInMemory failed");
        return {PatchStatus::kFailed, 0u};
    }

    if (!WriteAndCommitAtomically(cpk_path, patched_cpk.data(),
                                  patched_cpk.size())) {
        Log("MenuAutoPatch: atomic commit to sys.cpk failed (gle=%lu)",
            GetLastError());
        return {PatchStatus::kFailed, 0u};
    }

    if (!WriteSentinelAtomically(sentinel_path,
                                 hdr.patch_payload_hash,
                                 patch_file_size,
                                 patched_cpk.size())) {
        Log("MenuAutoPatch: warning — failed to write sys.cpk.applied sentinel");
    }

    Log("MenuAutoPatch: patched data\\sys.cpk (%llu -> %llu bytes, %u sub-textures)",
        static_cast<unsigned long long>(orig_cpk.size()),
        static_cast<unsigned long long>(patched_cpk.size()),
        static_cast<unsigned>(hdr.record_count));
    return {PatchStatus::kApplied, patched_cpk.size()};
}

PatchResult ApplyMenuPatchDetailed(const fs::path& game_dir) {
    const MenuAutoPatcher patcher;
    return patcher.Apply(game_dir);
}

bool ApplyMenuPatch(const fs::path& game_dir) {
    const PatchResult res = ApplyMenuPatchDetailed(game_dir);
    return res.status == PatchStatus::kApplied ||
           res.status == PatchStatus::kAlreadyUpToDate;
}

bool EnsureMenuArchivePatched() {
    std::vector<fs::path> candidates;

    const fs::path exe_dir = ParentDirOfModule(nullptr);
    if (!exe_dir.empty()) {
        candidates.push_back(exe_dir);
    }

    HMODULE self_mod = nullptr;
    if (GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&EnsureMenuArchivePatched),
            &self_mod) &&
        self_mod != nullptr) {
        const fs::path dll_dir = ParentDirOfModule(self_mod);
        if (!dll_dir.empty() &&
            std::find(candidates.begin(), candidates.end(), dll_dir) ==
                candidates.end()) {
            candidates.push_back(dll_dir);
        }
    }

    std::error_code ec;
    const fs::path cwd = fs::current_path(ec);
    if (!ec && !cwd.empty() &&
        std::find(candidates.begin(), candidates.end(), cwd) ==
            candidates.end()) {
        candidates.push_back(cwd);
    }

    for (const fs::path& dir : candidates) {
        std::uint64_t patch_sz = 0;
        if (!GetExactFileSize((dir / L"menu_patch.bin").wstring(), patch_sz)) {
            continue;
        }
        const std::wstring cpk = (dir / L"data" / L"sys.cpk").wstring();
        std::uint64_t dummy = 0;
        if (GetExactFileSize(cpk, dummy) ||
            GetExactFileSize(cpk + L".bak", dummy)) {
            return ApplyMenuPatch(dir);
        }
    }
    return false;
}

}  // namespace menu_autopatch
