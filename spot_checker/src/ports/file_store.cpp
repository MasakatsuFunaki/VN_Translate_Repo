// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "ports/file_store.h"

#include "common/utf8_text.h"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <system_error>
#include <utility>

namespace spot_checker {
namespace {

namespace fs = std::filesystem;

std::string path_display_utf8(const fs::path& path) {
    return wide_to_utf8(path.wstring());
}

}  // namespace

IFileStore::~IFileStore() = default;
DiskFileStore::~DiskFileStore() = default;

bool DiskFileStore::exists(const std::filesystem::path& path) const {
    std::error_code ec;
    return fs::exists(path, ec) && !ec;
}

bool DiskFileStore::is_directory(const std::filesystem::path& path) const {
    std::error_code ec;
    return fs::is_directory(path, ec) && !ec;
}

std::vector<std::filesystem::path> DiskFileStore::list_regular_files(
    const std::filesystem::path& dir) const {
    std::error_code ec;
    if (!fs::exists(dir, ec) || ec || !fs::is_directory(dir, ec) || ec) {
        return {};
    }

    std::vector<fs::path> files;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end;
         it.increment(ec)) {
        std::error_code entry_ec;
        if (it->is_regular_file(entry_ec) && !entry_ec) {
            files.push_back(it->path());
        }
    }

    std::sort(files.begin(), files.end(),
              [](const fs::path& lhs, const fs::path& rhs) {
                  return path_display_utf8(lhs.filename()) <
                         path_display_utf8(rhs.filename());
              });
    return files;
}

Result<std::string> DiskFileStore::read_text(
    const std::filesystem::path& path) const {
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) {
        return Result<std::string>::failure(
            ErrorCode::IoError, "file not found: " + path_display_utf8(path));
    }
    if (fs::is_directory(path, ec) && !ec) {
        return Result<std::string>::failure(
            ErrorCode::IoError,
            "path is a directory, not a regular file: " + path_display_utf8(path));
    }

    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return Result<std::string>::failure(
            ErrorCode::IoError, "cannot open file: " + path_display_utf8(path));
    }

    const std::streampos end_pos = in.tellg();
    if (end_pos < 0) {
        return Result<std::string>::failure(
            ErrorCode::IoError,
            "cannot query file size: " + path_display_utf8(path));
    }

    const auto byte_count = static_cast<std::size_t>(end_pos);
    std::string content(byte_count, '\0');
    in.seekg(0, std::ios::beg);
    if (byte_count > 0 &&
        !in.read(content.data(), static_cast<std::streamsize>(byte_count))) {
        return Result<std::string>::failure(
            ErrorCode::IoError, "failed reading file: " + path_display_utf8(path));
    }
    return Result<std::string>::success(std::move(content));
}

Status DiskFileStore::write_text_atomic(const std::filesystem::path& path,
                                        std::string_view content) {
    if (path.empty()) {
        return Status::error(ErrorCode::InvalidArgument,
                             "cannot write to empty file path");
    }

    std::error_code ec;
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path(), ec);
        if (ec) {
            return Status::error(ErrorCode::IoError,
                                 "cannot create parent directories for " +
                                     path_display_utf8(path) + ": " +
                                     ec.message());
        }
    }

    fs::path tmp_path = path;
    tmp_path += L".tmp";

    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            return Status::error(ErrorCode::IoError,
                                 "cannot open temporary file for writing: " +
                                     path_display_utf8(tmp_path));
        }
        if (!content.empty()) {
            out.write(content.data(),
                      static_cast<std::streamsize>(content.size()));
        }
        out.flush();
        if (!out) {
            out.close();
            std::error_code cleanup_ec;
            fs::remove(tmp_path, cleanup_ec);
            return Status::error(ErrorCode::IoError,
                                 "failed writing temporary file: " +
                                     path_display_utf8(tmp_path));
        }
    }

    fs::rename(tmp_path, path, ec);
    if (ec) {
        std::error_code cleanup_ec;
        fs::remove(tmp_path, cleanup_ec);
        return Status::error(ErrorCode::IoError,
                             "atomic rename failed for " +
                                 path_display_utf8(path) + ": " + ec.message());
    }
    return Status::ok();
}

}  // namespace spot_checker
