// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Separated from paths.h to keep <windows.h> out of every app main.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "apps/paths.h"

#include <filesystem>
#include <string>
#include <system_error>

namespace frat::apps {

namespace fs = std::filesystem;

// GetModuleFileNameW signals truncation by filling the buffer completely.
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
        buf.resize(buf.size() * 2);
    }
    return fs::path(buf).parent_path().u8string();
}

std::string default_project_dir() {
    const std::string exe = exe_dir();
    if (exe.empty()) return {};
    std::error_code ec;
    for (fs::path d = fs::u8path(exe); !d.empty(); d = d.parent_path()) {
        if (fs::is_directory(d / "pipeline_cpp", ec) &&
            fs::is_regular_file(d / "build.py", ec))
            return d.string();
        // Staged install: checked second so the checkout wins.
        if (fs::is_directory(d / "script_output", ec) &&
            fs::is_directory(d / "bin", ec))
            return d.string();
        if (d == d.root_path()) break;  // parent_path() of a root is itself
    }
    return {};
}

}  // namespace frat::apps
