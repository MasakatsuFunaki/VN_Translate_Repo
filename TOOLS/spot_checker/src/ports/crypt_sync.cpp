// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "ports/crypt_sync.h"

#include "common/utf8_text.h"
#include "common/win32_cli_env.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstddef>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace spot_checker {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kMaxCandidateScriptDirs = 4;
constexpr const char* kScriptOutputDir = "script_output";
constexpr const char* kTranslatedJsonName = "translated_text.json";
constexpr const char* kTranslatedEncName = "translated_text.json.enc";
constexpr const char* kDecryptBatName = "decrypt_translations.bat";
constexpr const char* kEncryptBatName = "encrypt_translations.bat";

struct UniqueWinHandle {
    UniqueWinHandle() noexcept = default;
    explicit UniqueWinHandle(HANDLE h) noexcept : handle(h) {}
    ~UniqueWinHandle() {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle);
        }
    }

    UniqueWinHandle(const UniqueWinHandle&) = delete;
    UniqueWinHandle& operator=(const UniqueWinHandle&) = delete;

    HANDLE handle = nullptr;
};

bool file_exists_noexcept(const fs::path& path) noexcept {
    std::error_code ec;
    return !path.empty() && fs::exists(path, ec) && !ec;
}

fs::path locate_batch_script(const fs::path& primary_dir,
                             const fs::path& game_root,
                             const fs::path& fallback_dir,
                             const char* script_name) {
    std::vector<fs::path> candidate_dirs;
    candidate_dirs.reserve(kMaxCandidateScriptDirs);
    if (!primary_dir.empty()) {
        candidate_dirs.push_back(primary_dir);
    }
    if (!game_root.empty()) {
        candidate_dirs.push_back(game_root);
        const fs::path parent = game_root.parent_path();
        if (!parent.empty() && parent != game_root) {
            candidate_dirs.push_back(parent);
        }
    }
    if (!fallback_dir.empty()) {
        candidate_dirs.push_back(fallback_dir);
    } else {
        const fs::path repo = find_repo_root();
        if (!repo.empty()) {
            candidate_dirs.push_back(repo);
        }
    }

    for (const fs::path& dir : candidate_dirs) {
        const fs::path candidate = dir / script_name;
        if (file_exists_noexcept(candidate)) {
            return candidate;
        }
    }
    return {};
}

Status run_batch_command(const fs::path& working_dir,
                         const std::wstring& command_line,
                         const char* step_label) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    const UniqueWinHandle nul_handle(::CreateFileW(
        L"NUL",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &sa,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr));

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    BOOL inherit_handles = FALSE;
    if (nul_handle.handle != nullptr && nul_handle.handle != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = nul_handle.handle;
        si.hStdOutput = nul_handle.handle;
        si.hStdError = nul_handle.handle;
        inherit_handles = TRUE;
    }

    PROCESS_INFORMATION pi{};
    std::wstring mutable_cmd = command_line;
    std::error_code ec;
    const std::wstring cwd =
        (!working_dir.empty() && fs::is_directory(working_dir, ec) && !ec)
            ? working_dir.wstring()
            : std::wstring{};

    const BOOL started = ::CreateProcessW(
        nullptr,
        mutable_cmd.data(),
        nullptr,
        nullptr,
        inherit_handles,
        CREATE_NO_WINDOW,
        nullptr,
        cwd.empty() ? nullptr : cwd.c_str(),
        &si,
        &pi);
    if (!started) {
        const DWORD last_err = ::GetLastError();
        return Status::error(ErrorCode::CryptSyncFailed,
                             std::string(step_label) +
                                 " CreateProcessW failed (Win32 error " +
                                 std::to_string(last_err) + ")");
    }

    const UniqueWinHandle proc_guard(pi.hProcess);
    const UniqueWinHandle thread_guard(pi.hThread);
    ::WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exit_code = 1;
    if (!::GetExitCodeProcess(pi.hProcess, &exit_code)) {
        return Status::error(ErrorCode::CryptSyncFailed,
                             std::string(step_label) +
                                 " GetExitCodeProcess failed");
    }
    if (exit_code != 0) {
        return Status::error(ErrorCode::CryptSyncFailed,
                             std::string(step_label) +
                                 " exited with non-zero code " +
                                 std::to_string(exit_code));
    }
    return Status::ok();
}

}  // namespace

ICryptSync::~ICryptSync() = default;
NoOpCryptSync::~NoOpCryptSync() = default;

Status NoOpCryptSync::decrypt_if_needed(const std::filesystem::path&) {
    return Status::ok();
}

Status NoOpCryptSync::encrypt_if_tracked(const std::filesystem::path&) {
    return Status::ok();
}

BatchProcessCryptSync::BatchProcessCryptSync(
    std::filesystem::path primary_scripts_dir,
    std::filesystem::path fallback_scripts_dir)
    : primary_scripts_dir_(std::move(primary_scripts_dir)),
      fallback_scripts_dir_(std::move(fallback_scripts_dir)) {}

BatchProcessCryptSync::~BatchProcessCryptSync() = default;

Status BatchProcessCryptSync::decrypt_if_needed(
    const std::filesystem::path& game_root) {
    if (game_root.empty()) {
        return Status::error(ErrorCode::InvalidArgument,
                             "decrypt_if_needed called with empty game_root");
    }

    const fs::path script_out = game_root / kScriptOutputDir;
    const fs::path plain_json = script_out / kTranslatedJsonName;
    const fs::path enc_json = script_out / kTranslatedEncName;

    if (file_exists_noexcept(plain_json) || !file_exists_noexcept(enc_json)) {
        return Status::ok();
    }

    fs::path bat_path = locate_batch_script(
        primary_scripts_dir_, game_root, fallback_scripts_dir_, kDecryptBatName);
    bool use_encrypt_bat_decrypt_flag = false;
    if (bat_path.empty()) {
        bat_path = locate_batch_script(
            primary_scripts_dir_, game_root, fallback_scripts_dir_, kEncryptBatName);
        use_encrypt_bat_decrypt_flag = !bat_path.empty();
    }
    if (bat_path.empty()) {
        return Status::error(
            ErrorCode::CryptSyncFailed,
            "decrypt_translations.bat not found for " +
                wide_to_utf8(game_root.wstring()));
    }

    const std::wstring mode_arg =
        use_encrypt_bat_decrypt_flag ? L" --decrypt --dir \"" : L" --dir \"";
    const std::wstring cmd =
        L"cmd.exe /d /s /c \"\"" + bat_path.wstring() + L"\"" + mode_arg +
        game_root.wstring() + L"\"\"";

    const Status run_status =
        run_batch_command(game_root, cmd, kDecryptBatName);
    if (!run_status.is_ok()) {
        return run_status;
    }
    if (!file_exists_noexcept(plain_json)) {
        return Status::error(
            ErrorCode::CryptSyncFailed,
            "decryption completed without materializing " +
                wide_to_utf8(plain_json.wstring()));
    }
    return Status::ok();
}

Status BatchProcessCryptSync::encrypt_if_tracked(
    const std::filesystem::path& game_root) {
    if (game_root.empty()) {
        return Status::error(ErrorCode::InvalidArgument,
                             "encrypt_if_tracked called with empty game_root");
    }

    const fs::path script_out = game_root / kScriptOutputDir;
    const fs::path plain_json = script_out / kTranslatedJsonName;
    const fs::path enc_json = script_out / kTranslatedEncName;

    if (!file_exists_noexcept(enc_json) || !file_exists_noexcept(plain_json)) {
        return Status::ok();
    }

    const fs::path enc_bat = locate_batch_script(
        primary_scripts_dir_, game_root, fallback_scripts_dir_, kEncryptBatName);
    if (enc_bat.empty()) {
        return Status::error(
            ErrorCode::CryptSyncFailed,
            "encrypt_translations.bat not found for " +
                wide_to_utf8(game_root.wstring()));
    }

    const std::wstring cmd =
        L"cmd.exe /d /s /c \"\"" + enc_bat.wstring() +
        L"\" --encrypt --dir \"" + game_root.wstring() + L"\"\"";
    return run_batch_command(game_root, cmd, kEncryptBatName);
}

std::unique_ptr<ICryptSync> make_batch_crypt_sync(
    std::filesystem::path primary_scripts_dir,
    std::filesystem::path fallback_scripts_dir) {
    return std::make_unique<BatchProcessCryptSync>(
        std::move(primary_scripts_dir), std::move(fallback_scripts_dir));
}

}  // namespace spot_checker
