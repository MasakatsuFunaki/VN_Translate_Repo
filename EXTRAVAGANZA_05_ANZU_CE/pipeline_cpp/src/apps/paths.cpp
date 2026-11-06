// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

#include "apps/paths.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <filesystem>
#include <string>
#include <system_error>

namespace exm::apps {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kBufferGrowthFactor = 2;

}  // namespace

std::string exe_dir() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n =
            GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * kBufferGrowthFactor);
    }
    return fs::path(buf).parent_path().u8string();
}

std::string find_project_dir_from(const std::string& start_dir) {
    if (start_dir.empty()) return {};
    std::error_code ec;
    for (fs::path d = fs::u8path(start_dir); !d.empty(); d = d.parent_path()) {
        if (fs::is_directory(d / "pipeline_cpp", ec) &&
            fs::is_regular_file(d / "build.py", ec)) {
            return d.string();
        }
        if (fs::is_directory(d / "script_output", ec) &&
            fs::is_directory(d / "bin", ec)) {
            return d.string();
        }
        if (d == d.root_path()) break;
    }
    return {};
}

std::string default_project_dir() {
    return find_project_dir_from(exe_dir());
}

}  // namespace exm::apps
