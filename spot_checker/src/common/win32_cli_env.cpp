// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "common/win32_cli_env.h"

#include "common/utf8_text.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <cstddef>
#include <system_error>

namespace spot_checker {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kInitialModulePathChars = MAX_PATH;
constexpr std::size_t kModulePathGrowthFactor = 2;
constexpr std::size_t kMaxModulePathChars = 32768;

struct LocalFreeGuard {
    explicit LocalFreeGuard(wchar_t** ptr) noexcept : argv(ptr) {}
    ~LocalFreeGuard() {
        if (argv != nullptr) {
            ::LocalFree(argv);
        }
    }

    LocalFreeGuard(const LocalFreeGuard&) = delete;
    LocalFreeGuard& operator=(const LocalFreeGuard&) = delete;

    wchar_t** argv = nullptr;
};

fs::path path_from_utf8(std::string_view utf8) {
    return fs::path(utf8_to_wide(utf8));
}

std::string path_to_utf8(const fs::path& path) {
    return wide_to_utf8(path.wstring());
}

}  // namespace

void setup_console_utf8() noexcept {
    ::SetConsoleOutputCP(CP_UTF8);
    ::SetConsoleCP(CP_UTF8);
}

void lower_process_priority() noexcept {
    ::SetPriorityClass(::GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
}

std::filesystem::path executable_path() {
    std::wstring buffer(kInitialModulePathChars, L'\0');
    while (buffer.size() <= kMaxModulePathChars) {
        const DWORD copied = ::GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0) {
            return {};
        }
        if (copied < buffer.size()) {
            buffer.resize(copied);
            return fs::path(buffer);
        }
        buffer.resize(buffer.size() * kModulePathGrowthFactor, L'\0');
    }
    return {};
}

std::vector<std::string> process_utf8_args() {
    const wchar_t* cmd_line = ::GetCommandLineW();
    if (cmd_line == nullptr || *cmd_line == L'\0') {
        return {};
    }

    int argc = 0;
    wchar_t** argv = ::CommandLineToArgvW(cmd_line, &argc);
    const LocalFreeGuard guard(argv);
    if (argv == nullptr || argc <= 1) {
        return {};
    }

    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc - 1));
    for (int i = 1; i < argc; ++i) {
        args.push_back(wide_to_utf8(argv[i]));
    }
    return args;
}

bool is_repo_root_dir(const std::filesystem::path& candidate) noexcept {
    try {
        if (candidate.empty()) {
            return false;
        }
        std::error_code ec;
        if (!fs::is_directory(candidate, ec) || ec) {
            return false;
        }
        const fs::path spot_checker_dir = candidate / "spot_checker";
        if (fs::is_regular_file(spot_checker_dir / "CMakeLists.txt", ec) && !ec) {
            return true;
        }
        ec.clear();
        if (!fs::is_directory(spot_checker_dir, ec) || ec) {
            return false;
        }
        ec.clear();
        if (fs::is_regular_file(candidate / "encrypt_translations.bat", ec) && !ec) {
            return true;
        }
        ec.clear();
        return fs::is_regular_file(candidate / "build.py", ec) && !ec;
    } catch (...) {
        return false;
    }
}

std::filesystem::path find_repo_root_from(
    const std::filesystem::path& start_path,
    const std::filesystem::path& fallback_dir) {
    if (start_path.empty()) {
        return fallback_dir;
    }

    std::error_code ec;
    fs::path current = fs::absolute(start_path, ec);
    if (ec || current.empty()) {
        current = start_path;
    }
    current = current.lexically_normal();
    if (!current.has_filename() && current != current.root_path()) {
        current = current.parent_path();
    }
    ec.clear();
    if (fs::is_regular_file(current, ec) && !ec) {
        current = current.parent_path();
    }

    while (!current.empty()) {
        if (is_repo_root_dir(current)) {
            return current;
        }
        const fs::path parent = current.parent_path();
        if (parent.empty() || parent == current) {
            break;
        }
        current = parent;
    }
    return fallback_dir;
}

std::filesystem::path find_repo_root() {
    std::error_code ec;
    return find_repo_root_from(executable_path(), fs::current_path(ec));
}

std::string resolve_game_dir(std::string_view game_dir_opt,
                             std::string_view game_name_opt,
                             const std::filesystem::path& repo_root) {
    std::error_code ec;
    if (!game_dir_opt.empty()) {
        const fs::path raw = path_from_utf8(game_dir_opt);
        const fs::path abs_path = fs::absolute(raw, ec);
        return path_to_utf8(ec ? raw : abs_path);
    }
    if (game_name_opt.empty()) {
        return {};
    }

    const fs::path game_rel = path_from_utf8(game_name_opt);
    if (!repo_root.empty()) {
        const fs::path repo_candidate = repo_root / game_rel;
        if (fs::is_directory(repo_candidate, ec) && !ec) {
            const fs::path abs_candidate = fs::absolute(repo_candidate, ec);
            return path_to_utf8(ec ? repo_candidate : abs_candidate);
        }
        ec.clear();
    }

    const fs::path abs_fallback = fs::absolute(game_rel, ec);
    return path_to_utf8(ec ? game_rel : abs_fallback);
}

}  // namespace spot_checker
