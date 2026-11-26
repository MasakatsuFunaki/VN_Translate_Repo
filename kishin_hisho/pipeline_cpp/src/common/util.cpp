// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "util.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace ksh {

namespace {

constexpr DWORD kMaxIoChunkBytes = 16u * 1024u * 1024u;
constexpr std::size_t kCopyStreamChunkBytes = 1024u * 1024u;
constexpr wchar_t kTempSuffix[] = L".tmp";
constexpr wchar_t kBackupSuffix[] = L".bak";
constexpr wchar_t kBackupTempSuffix[] = L".bak.tmp";

class ScopedWinHandle {
public:
    explicit ScopedWinHandle(HANDLE h = INVALID_HANDLE_VALUE) noexcept : handle_(h) {}
    ~ScopedWinHandle() { Reset(); }

    ScopedWinHandle(const ScopedWinHandle&) = delete;
    ScopedWinHandle& operator=(const ScopedWinHandle&) = delete;
    ScopedWinHandle(ScopedWinHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}
    ScopedWinHandle& operator=(ScopedWinHandle&& other) noexcept {
        if (this != &other) {
            Reset();
            handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }

    HANDLE Get() const noexcept { return handle_; }
    bool Valid() const noexcept { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }

    void Reset() noexcept {
        if (Valid()) {
            ::CloseHandle(handle_);
        }
        handle_ = INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_;
};

void ClearReadOnlyAttribute(const std::wstring& path) noexcept {
    const DWORD attrs = ::GetFileAttributesW(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY) != 0u) {
        ::SetFileAttributesW(path.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
    }
}

bool RegularFileExists(const std::wstring& path) noexcept {
    const DWORD attrs = ::GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0u;
}

std::wstring NormalizePathKey(const std::wstring& path) {
    const std::wstring normalized = std::filesystem::path(path).lexically_normal().wstring();
    if (normalized.empty()) {
        return normalized;
    }
    std::wstring upper(normalized.size(), L'\0');
    const int written =
        ::LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, normalized.data(),
                        static_cast<int>(normalized.size()), upper.data(),
                        static_cast<int>(upper.size()), nullptr, nullptr, 0);
    if (written != static_cast<int>(normalized.size())) {
        return normalized;
    }
    return upper;
}

bool WriteExact(HANDLE file, const void* data, std::size_t size) noexcept {
    const auto* src = static_cast<const std::uint8_t*>(data);
    std::size_t done = 0;
    while (done < size) {
        const DWORD chunk =
            static_cast<DWORD>(std::min<std::size_t>(size - done, kMaxIoChunkBytes));
        DWORD written = 0;
        if (!::WriteFile(file, src + done, chunk, &written, nullptr) || written != chunk) {
            return false;
        }
        done += static_cast<std::size_t>(written);
    }
    return true;
}

class ScopedTempFile {
public:
    ScopedTempFile() = default;
    ~ScopedTempFile() { Cleanup(); }

    ScopedTempFile(const ScopedTempFile&) = delete;
    ScopedTempFile& operator=(const ScopedTempFile&) = delete;
    ScopedTempFile(ScopedTempFile&& other) noexcept
        : path_(std::move(other.path_)), active_(std::exchange(other.active_, false)) {}
    ScopedTempFile& operator=(ScopedTempFile&& other) noexcept {
        if (this != &other) {
            Cleanup();
            path_ = std::move(other.path_);
            active_ = std::exchange(other.active_, false);
        }
        return *this;
    }

    void Arm(std::wstring path) {
        Cleanup();
        path_ = std::move(path);
        active_ = true;
    }

    bool CommitMove(const std::wstring& dest_path, bool replace_existing) noexcept {
        if (!active_) {
            return false;
        }
        if (replace_existing) {
            ClearReadOnlyAttribute(dest_path);
        }
        const DWORD flags = replace_existing
                                ? (MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
                                : MOVEFILE_WRITE_THROUGH;
        if (!::MoveFileExW(path_.c_str(), dest_path.c_str(), flags)) {
            return false;
        }
        active_ = false;
        return true;
    }

    void Cleanup() noexcept {
        if (!active_) {
            return;
        }
        ClearReadOnlyAttribute(path_);
        ::DeleteFileW(path_.c_str());
        active_ = false;
    }

private:
    std::wstring path_;
    bool active_ = false;
};

void StageTempBytes(const std::wstring& tmp_path,
                    const void* data,
                    std::size_t size,
                    const std::string& label,
                    ScopedTempFile& guard_out) {
    ClearReadOnlyAttribute(tmp_path);
    guard_out.Arm(tmp_path);

    ScopedWinHandle file(::CreateFileW(tmp_path.c_str(), GENERIC_WRITE, 0u, nullptr, CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.Valid()) {
        guard_out.Cleanup();
        throw std::runtime_error("cannot create " + label);
    }
    if ((size > 0 && !WriteExact(file.Get(), data, size)) || !::FlushFileBuffers(file.Get())) {
        file.Reset();
        guard_out.Cleanup();
        throw std::runtime_error("write failed: " + label);
    }
}

void StageTempCopy(const std::wstring& src_path,
                   const std::wstring& tmp_path,
                   const std::string& src_label,
                   const std::string& dst_label,
                   ScopedTempFile& guard_out) {
    ScopedWinHandle src(::CreateFileW(
        src_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!src.Valid()) {
        throw std::runtime_error("cannot open " + src_label);
    }

    ClearReadOnlyAttribute(tmp_path);
    guard_out.Arm(tmp_path);

    ScopedWinHandle dst(::CreateFileW(tmp_path.c_str(), GENERIC_WRITE, 0u, nullptr, CREATE_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!dst.Valid()) {
        guard_out.Cleanup();
        throw std::runtime_error("cannot create " + dst_label);
    }

    Bytes buf(kCopyStreamChunkBytes);
    for (;;) {
        DWORD got = 0;
        if (!::ReadFile(src.Get(), buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) {
            dst.Reset();
            guard_out.Cleanup();
            throw std::runtime_error("read failed: " + src_label);
        }
        if (got == 0) {
            break;
        }
        if (!WriteExact(dst.Get(), buf.data(), static_cast<std::size_t>(got))) {
            dst.Reset();
            guard_out.Cleanup();
            throw std::runtime_error("write failed: " + dst_label);
        }
    }
    if (!::FlushFileBuffers(dst.Get())) {
        dst.Reset();
        guard_out.Cleanup();
        throw std::runtime_error("flush failed: " + dst_label);
    }
}

}  // namespace

struct AtomicFileBatch::Impl {
    struct Entry {
        std::string label;
        std::wstring normalized_key;
        std::wstring dst_path;
        std::wstring backup_path;
        ScopedTempFile staged_target;
        ScopedTempFile staged_backup;
        bool needs_backup = false;
    };

    bool ContainsKey(const std::wstring& key) const noexcept {
        return std::any_of(entries.begin(), entries.end(),
                           [&key](const Entry& e) { return e.normalized_key == key; });
    }

    std::vector<Entry> entries;
};

AtomicFileBatch::AtomicFileBatch() : impl_(std::make_unique<Impl>()) {}
AtomicFileBatch::~AtomicFileBatch() = default;
AtomicFileBatch::AtomicFileBatch(AtomicFileBatch&&) noexcept = default;
AtomicFileBatch& AtomicFileBatch::operator=(AtomicFileBatch&&) noexcept = default;

void AtomicFileBatch::StageWrite(const std::string& dst_path,
                                 const void* data,
                                 std::size_t size,
                                 bool backup_existing) {
    if (!impl_) {
        throw std::logic_error("AtomicFileBatch::StageWrite called on moved-from batch");
    }
    Impl::Entry entry;
    entry.label = dst_path;
    entry.dst_path = std::filesystem::u8path(dst_path).wstring();
    entry.normalized_key = NormalizePathKey(entry.dst_path);
    if (impl_->ContainsKey(entry.normalized_key)) {
        throw std::invalid_argument("duplicate staged destination path: " + dst_path);
    }
    entry.backup_path = entry.dst_path + kBackupSuffix;

    if (backup_existing && RegularFileExists(entry.dst_path) &&
        !RegularFileExists(entry.backup_path)) {
        StageTempCopy(entry.dst_path, entry.dst_path + kBackupTempSuffix, dst_path,
                      dst_path + ".bak", entry.staged_backup);
        entry.needs_backup = true;
    }

    StageTempBytes(entry.dst_path + kTempSuffix, data, size, dst_path, entry.staged_target);
    impl_->entries.push_back(std::move(entry));
}

void AtomicFileBatch::StageWrite(const std::string& dst_path,
                                 const Bytes& data,
                                 bool backup_existing) {
    StageWrite(dst_path, data.data(), data.size(), backup_existing);
}

void AtomicFileBatch::StageCopy(const std::string& src_path,
                                const std::string& dst_path,
                                bool backup_existing) {
    if (!impl_) {
        throw std::logic_error("AtomicFileBatch::StageCopy called on moved-from batch");
    }
    Impl::Entry entry;
    entry.label = dst_path;
    entry.dst_path = std::filesystem::u8path(dst_path).wstring();
    entry.normalized_key = NormalizePathKey(entry.dst_path);
    if (impl_->ContainsKey(entry.normalized_key)) {
        throw std::invalid_argument("duplicate staged destination path: " + dst_path);
    }
    entry.backup_path = entry.dst_path + kBackupSuffix;
    const std::wstring wide_src = std::filesystem::u8path(src_path).wstring();

    if (backup_existing && RegularFileExists(entry.dst_path) &&
        !RegularFileExists(entry.backup_path)) {
        StageTempCopy(entry.dst_path, entry.dst_path + kBackupTempSuffix, dst_path,
                      dst_path + ".bak", entry.staged_backup);
        entry.needs_backup = true;
    }

    StageTempCopy(wide_src, entry.dst_path + kTempSuffix, src_path, dst_path, entry.staged_target);
    impl_->entries.push_back(std::move(entry));
}

void AtomicFileBatch::Commit() {
    if (!impl_) {
        throw std::logic_error("AtomicFileBatch::Commit called on moved-from batch");
    }
    for (Impl::Entry& entry : impl_->entries) {
        if (!entry.needs_backup) {
            continue;
        }
        if (RegularFileExists(entry.backup_path)) {
            entry.staged_backup.Cleanup();
            continue;
        }
        if (!entry.staged_backup.CommitMove(entry.backup_path, false)) {
            throw std::runtime_error("backup commit failed: " + entry.label + ".bak");
        }
    }
    for (Impl::Entry& entry : impl_->entries) {
        if (!entry.staged_target.CommitMove(entry.dst_path, true)) {
            throw std::runtime_error("replace failed: " + entry.label);
        }
    }
    impl_->entries.clear();
}

// ---- file IO ---------------------------------------------------------------

Bytes read_file(const std::string& path) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    f.seekg(0, std::ios::end);
    const auto size = static_cast<std::size_t>(f.tellg());
    f.seekg(0);
    Bytes out(size);
    if (size) f.read(reinterpret_cast<char*>(out.data()), size);
    if (!f) throw std::runtime_error("read failed: " + path);
    return out;
}

void write_file(const std::string& path, const void* data, std::size_t size) {
    const std::wstring wide_path = std::filesystem::u8path(path).wstring();
    ScopedTempFile staged;
    StageTempBytes(wide_path + kTempSuffix, data, size, path, staged);
    if (!staged.CommitMove(wide_path, true)) {
        throw std::runtime_error("replace failed: " + path);
    }
}

void copy_file_atomic(const std::string& src_path, const std::string& dst_path) {
    const std::wstring wide_src = std::filesystem::u8path(src_path).wstring();
    const std::wstring wide_dst = std::filesystem::u8path(dst_path).wstring();
    ScopedTempFile staged;
    StageTempCopy(wide_src, wide_dst + kTempSuffix, src_path, dst_path, staged);
    if (!staged.CommitMove(wide_dst, true)) {
        throw std::runtime_error("replace failed: " + dst_path);
    }
}

bool backup_file_if_missing(const std::string& target_path) {
    const std::wstring wide_target = std::filesystem::u8path(target_path).wstring();
    const std::wstring wide_backup = wide_target + kBackupSuffix;
    if (!RegularFileExists(wide_target) || RegularFileExists(wide_backup)) {
        return false;
    }
    ScopedTempFile staged;
    StageTempCopy(wide_target, wide_target + kBackupTempSuffix, target_path,
                  target_path + ".bak", staged);
    if (RegularFileExists(wide_backup)) {
        staged.Cleanup();
        return false;
    }
    if (!staged.CommitMove(wide_backup, false)) {
        throw std::runtime_error("backup commit failed: " + target_path + ".bak");
    }
    return true;
}

void write_file_text(const std::string& path, const std::string& content) {
    std::string out;
    out.reserve(content.size() + content.size() / 16);
    for (std::size_t i = 0; i < content.size(); ++i) {
        // Never double up an existing CRLF.
        if (content[i] == '\n' && (i == 0 || content[i - 1] != '\r')) out += '\r';
        out += content[i];
    }
    write_file(path, out);
}

// ---- logging ---------------------------------------------------------------

namespace {
std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}
}  // namespace

void log_info(const std::string& msg) {
    std::fputs((timestamp() + " INFO " + msg + "\n").c_str(), stdout);
    std::fflush(stdout);
}

void print_line(const std::string& msg) {
    std::fputs((msg + "\n").c_str(), stdout);
    std::fflush(stdout);
}

void setup_console_utf8() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}

// ---- formatting ------------------------------------------------------------

std::string hexu(std::uint64_t v, int width) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%0*llX", width, static_cast<unsigned long long>(v));
    return buf;
}

std::string hexl(std::uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}

std::string hexu_signed(std::int64_t v, int width) {
    const bool neg = v < 0;
    const auto mag = static_cast<unsigned long long>(neg ? -v : v);
    char buf[32];
    // The sign occupies one column of the padded field, so the magnitude gets
    // one digit less than `width` when negative.
    std::snprintf(buf, sizeof(buf), "%s%0*llX", neg ? "-" : "", neg ? width - 1 : width, mag);
    return buf;
}

std::string hex_bytes(const std::uint8_t* p, std::size_t n) {
    std::string out;
    out.reserve(n * 3);
    for (std::size_t i = 0; i < n; ++i) {
        if (i) out += ' ';
        char buf[4];
        std::snprintf(buf, sizeof(buf), "%02X", p[i]);
        out += buf;
    }
    return out;
}

std::string hex_lower(const std::uint8_t* p, std::size_t n) {
    std::string out;
    out.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        char buf[4];
        std::snprintf(buf, sizeof(buf), "%02x", p[i]);
        out += buf;
    }
    return out;
}

std::string ljust(const std::string& s, std::size_t width) {
    if (s.size() >= width) return s;
    return s + std::string(width - s.size(), ' ');
}

std::string bytes_repr(const std::uint8_t* p, std::size_t n) {
    const bool has_sq = std::find(p, p + n, '\'') != p + n;
    const bool has_dq = std::find(p, p + n, '"') != p + n;
    const char quote = (has_sq && !has_dq) ? '"' : '\'';
    std::string out;
    out += 'b';
    out += quote;
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t c = p[i];
        if (c == static_cast<std::uint8_t>(quote) || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c == '\t') {
            out += "\\t";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c < 0x20 || c >= 0x7F) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\x%02x", c);
            out += buf;
        } else {
            out += static_cast<char>(c);
        }
    }
    out += quote;
    return out;
}

}  // namespace ksh
