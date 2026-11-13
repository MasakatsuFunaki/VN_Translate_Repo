// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// RAII temporary directory fixture for unit tests.
#pragma once

#include <atomic>
#include <filesystem>
#include <random>
#include <string>
#include <string_view>
#include <system_error>

namespace mgi::test_util {

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view prefix)
        : path_(make_unique_path(prefix)) {
        std::filesystem::create_directories(path_);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    ~ScopedTempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

    std::filesystem::path join(std::string_view rel_utf8) const {
        return path_ / std::filesystem::u8path(rel_utf8);
    }

    std::string join_u8(std::string_view rel_utf8) const {
        return join(rel_utf8).u8string();
    }

private:
    static std::filesystem::path make_unique_path(std::string_view prefix) {
        static const unsigned process_nonce = std::random_device{}();
        static std::atomic<unsigned> next_id{0u};
        const unsigned id = next_id.fetch_add(1u, std::memory_order_relaxed);
        const std::string dir_name =
            std::string(prefix) + "_" + std::to_string(process_nonce) + "_" +
            std::to_string(id);
        return std::filesystem::temp_directory_path() /
               std::filesystem::u8path(dir_name);
    }

    std::filesystem::path path_;
};

}  // namespace mgi::test_util
