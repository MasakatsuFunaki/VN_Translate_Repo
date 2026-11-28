// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once

#include "common/status.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace spot_checker {

// Pure protocol interface for filesystem access and atomic file replacement.
// Enables deterministic mid-batch write-fault injection in component tests
// without OS file-locking hacks.
class IFileStore {
public:
    virtual ~IFileStore();

    IFileStore(const IFileStore&) = delete;
    IFileStore& operator=(const IFileStore&) = delete;

    virtual bool exists(const std::filesystem::path& path) const = 0;
    virtual bool is_directory(const std::filesystem::path& path) const = 0;

    // Lists immediate regular files in `dir`, sorted lexicographically by UTF-8
    // filename (`path.filename().u8string()`). Returns an empty vector if `dir`
    // does not exist.
    virtual std::vector<std::filesystem::path> list_regular_files(
        const std::filesystem::path& dir) const = 0;

    // Reads full binary/UTF-8 bytes of `path` without newline translation.
    // Returns `ErrorCode::IoError` if `path` does not exist or cannot be read.
    virtual Result<std::string> read_text(
        const std::filesystem::path& path) const = 0;

    // Writes `content` to a sibling `<path>.tmp` file (creating parent
    // directories if needed) and atomically renames over `path`. Removes
    // `<path>.tmp` on failure and returns `ErrorCode::IoError`.
    virtual Status write_text_atomic(const std::filesystem::path& path,
                                     std::string_view content) = 0;

protected:
    IFileStore() = default;
    IFileStore(IFileStore&&) noexcept = default;
    IFileStore& operator=(IFileStore&&) noexcept = default;
};

class DiskFileStore final : public IFileStore {
public:
    DiskFileStore() = default;
    ~DiskFileStore() override;

    bool exists(const std::filesystem::path& path) const override;
    bool is_directory(const std::filesystem::path& path) const override;
    std::vector<std::filesystem::path> list_regular_files(
        const std::filesystem::path& dir) const override;
    Result<std::string> read_text(
        const std::filesystem::path& path) const override;
    Status write_text_atomic(const std::filesystem::path& path,
                             std::string_view content) override;
};

}  // namespace spot_checker
