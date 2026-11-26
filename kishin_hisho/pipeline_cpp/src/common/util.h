// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Shared helpers for the kishin_hisho tooling: file IO, logging, and the report
// formatters.
//
// This game is NOT a translation pipeline -- it is a DRM/compat patcher plus a
// set of RE forensics tools -- so the CP932 codec, the JSON writers and the
// Japanese classifiers the other games carry are all absent here.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ksh {

using Bytes = std::vector<std::uint8_t>;

// ---- file IO ---------------------------------------------------------------

Bytes read_file(const std::string& path);

// Writes `data[0..size)` atomically via `<path>.tmp`, FlushFileBuffers, and
// MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH). Cleans up
// `<path>.tmp` on failure without mutating any existing file at `path`.
void write_file(const std::string& path, const void* data, std::size_t size);
inline void write_file(const std::string& path, const Bytes& b) {
    write_file(path, b.data(), b.size());
}
inline void write_file(const std::string& path, const std::string& s) {
    write_file(path, s.data(), s.size());
}

// Copies `src_path` to `dst_path` atomically via `<dst_path>.tmp`,
// FlushFileBuffers, and MoveFileExW. Cleans up `.tmp` on failure.
void copy_file_atomic(const std::string& src_path, const std::string& dst_path);

// If `target_path` is an existing regular file and `<target_path>.bak` does
// not yet exist, creates an immutable `<target_path>.bak` atomically via
// `<target_path>.bak.tmp` + FlushFileBuffers + MoveFileExW (without replacing
// an existing `.bak`). Returns true iff a new `.bak` was created.
bool backup_file_if_missing(const std::string& target_path);

// Two-phase atomic file writer/copier: StageWrite/StageCopy flush every
// `<dst>.tmp` (and `<dst>.bak.tmp` when `backup_existing` is true and `<dst>`
// exists without `<dst>.bak`) before Commit() promotes `.bak` files and swaps
// live targets. Uncommitted temp files are deleted automatically on scope exit.
// Thread-compatible.
class AtomicFileBatch {
public:
    AtomicFileBatch();
    ~AtomicFileBatch();

    AtomicFileBatch(const AtomicFileBatch&) = delete;
    AtomicFileBatch& operator=(const AtomicFileBatch&) = delete;
    AtomicFileBatch(AtomicFileBatch&&) noexcept;
    AtomicFileBatch& operator=(AtomicFileBatch&&) noexcept;

    void StageWrite(const std::string& dst_path,
                    const void* data,
                    std::size_t size,
                    bool backup_existing = false);
    void StageWrite(const std::string& dst_path,
                    const Bytes& data,
                    bool backup_existing = false);
    void StageCopy(const std::string& src_path,
                   const std::string& dst_path,
                   bool backup_existing = false);

    void Commit();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Writes `content` with CRLF line endings.  The committed analysis logs are the
// regression fixtures for the report builders and they hold CRLF, so the line
// endings are part of the contract, not a formatting preference.
void write_file_text(const std::string& path, const std::string& content);

// ---- logging ---------------------------------------------------------------

// Progress line: "HH:MM:SS INFO <message>" on stdout.
void log_info(const std::string& msg);
// Bare line, no timestamp -- report bodies and anything the logs capture.
void print_line(const std::string& msg);

// Set the console to UTF-8.  Every tool must call this before printing anything:
// without it the em dash in 01_recon's no-imports line and the arrows in 03 come
// out mojibake.
void setup_console_utf8();

// ---- formatting ------------------------------------------------------------

// Uppercase hex, zero-padded to `width` (0 = no padding); never truncates.
std::string hexu(std::uint64_t v, int width);
// Lowercase hex with an "0x" prefix and no padding.
std::string hexl(std::uint64_t v);
// Uppercase hex for a SIGNED value: the sign is part of the padded field, so -5
// at width 8 renders as "-0000005", not the two's-complement form.
std::string hexu_signed(std::int64_t v, int width);
// "AA BB CC" -- space-separated uppercase hex bytes.
std::string hex_bytes(const std::uint8_t* p, std::size_t n);
inline std::string hex_bytes(const Bytes& b) { return hex_bytes(b.data(), b.size()); }
// "aabbcc" -- lowercase, no separators.
std::string hex_lower(const std::uint8_t* p, std::size_t n);
// Left-justified to `width`; never truncates.
std::string ljust(const std::string& s, std::size_t width);
// Byte-string literal form: b'...' with \t \n \r \\ \' escapes, \xNN for the
// rest, and double quotes when the data contains a single quote but no double
// quote.  02_decrypt_devnames prints the decrypted table with it.
std::string bytes_repr(const std::uint8_t* p, std::size_t n);
inline std::string bytes_repr(const Bytes& b) { return bytes_repr(b.data(), b.size()); }

}  // namespace ksh
