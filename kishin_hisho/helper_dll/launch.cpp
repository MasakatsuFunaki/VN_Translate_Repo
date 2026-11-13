// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// 32-bit launcher: same architecture as the target process so kernel32
// LoadLibraryW sits at the identical address in both processes. Spawns
// KishinHisho.exe suspended, injects alpharom_helper.dll via wide
// LoadLibraryW, resumes the main thread, waits for exit, and prints the log.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cwchar>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr DWORD kInitialModulePathChars = MAX_PATH;
constexpr DWORD kMaxModulePathChars = 32768U;
constexpr std::size_t kLogReadChunkBytes = 4096U;
constexpr const wchar_t* kDefaultTargetExe =
    L"C:\\Program Files (x86)\\NitroPlus\\KishinHisho\\KishinHisho.exe";

class LocalFreeGuard {
public:
    explicit LocalFreeGuard(wchar_t** argv) noexcept : argv_(argv) {}
    ~LocalFreeGuard() {
        if (argv_ != nullptr) {
            ::LocalFree(argv_);
        }
    }
    LocalFreeGuard(const LocalFreeGuard&) = delete;
    LocalFreeGuard& operator=(const LocalFreeGuard&) = delete;

private:
    wchar_t** argv_ = nullptr;
};

class ScopedHandle {
public:
    ScopedHandle() noexcept = default;
    explicit ScopedHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~ScopedHandle() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle_);
        }
    }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    HANDLE get() const noexcept { return handle_; }

private:
    HANDLE handle_ = nullptr;
};

class ProcessTerminateGuard {
public:
    explicit ProcessTerminateGuard(HANDLE process) noexcept
        : process_(process), active_(process != nullptr && process != INVALID_HANDLE_VALUE) {}
    ~ProcessTerminateGuard() {
        if (active_) {
            ::TerminateProcess(process_, 1U);
        }
    }
    ProcessTerminateGuard(const ProcessTerminateGuard&) = delete;
    ProcessTerminateGuard& operator=(const ProcessTerminateGuard&) = delete;

    void Release() noexcept { active_ = false; }

private:
    HANDLE process_ = nullptr;
    bool active_ = false;
};

class RemoteMemoryGuard {
public:
    RemoteMemoryGuard(HANDLE process, LPVOID address) noexcept
        : process_(process), address_(address) {}
    ~RemoteMemoryGuard() {
        if (process_ != nullptr && process_ != INVALID_HANDLE_VALUE && address_ != nullptr) {
            ::VirtualFreeEx(process_, address_, 0U, MEM_RELEASE);
        }
    }
    RemoteMemoryGuard(const RemoteMemoryGuard&) = delete;
    RemoteMemoryGuard& operator=(const RemoteMemoryGuard&) = delete;

private:
    HANDLE process_ = nullptr;
    LPVOID address_ = nullptr;
};

std::string WideToUtf8(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int needed = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
        nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    const int written = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()),
        out.data(), needed, nullptr, nullptr);
    if (written != needed) {
        return {};
    }
    return out;
}

std::wstring GetCurrentModuleSiblingPathW(const wchar_t* sibling_name) {
    std::vector<wchar_t> buf(kInitialModulePathChars, L'\0');
    DWORD len = 0;
    for (;;) {
        ::SetLastError(ERROR_SUCCESS);
        len = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0) {
            return {};
        }
        if (len < buf.size() - 1U && ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            break;
        }
        if (buf.size() >= kMaxModulePathChars) {
            return {};
        }
        const std::size_t next_size =
            std::min<std::size_t>(buf.size() * 2U, kMaxModulePathChars);
        buf.assign(next_size, L'\0');
    }

    std::wstring path(buf.data(), len);
    const std::size_t sep = path.find_last_of(L"\\/");
    if (sep != std::wstring::npos) {
        path.resize(sep + 1U);
        path += sibling_name;
        return path;
    }
    return std::wstring(sibling_name);
}

std::wstring ReplaceFilenameW(const std::wstring& exe_path, const wchar_t* file_name) {
    const std::size_t sep = exe_path.find_last_of(L"\\/");
    if (sep == std::wstring::npos) {
        return std::wstring(file_name);
    }
    return exe_path.substr(0U, sep + 1U) + file_name;
}

std::wstring ExtractParentDirW(const std::wstring& exe_path) {
    const std::size_t sep = exe_path.find_last_of(L"\\/");
    if (sep == std::wstring::npos) {
        return {};
    }
    return exe_path.substr(0U, sep);
}

int RunLauncher() {
    ::SetConsoleOutputCP(CP_UTF8);

    const wchar_t* raw_cmd = ::GetCommandLineW();
    int wargc = 0;
    wchar_t** wargv = (raw_cmd != nullptr && *raw_cmd != L'\0')
                          ? ::CommandLineToArgvW(raw_cmd, &wargc)
                          : nullptr;
    const LocalFreeGuard argv_guard(wargv);
    if (raw_cmd != nullptr && *raw_cmd != L'\0' && wargv == nullptr) {
        std::printf("CommandLineToArgvW failed: %lu\n", ::GetLastError());
        return 1;
    }

    const std::wstring exe_path_w =
        (wargv != nullptr && wargc > 1 && wargv[1] != nullptr && *wargv[1] != L'\0')
            ? std::wstring(wargv[1])
            : std::wstring(kDefaultTargetExe);

    const std::wstring dll_path_w = GetCurrentModuleSiblingPathW(L"alpharom_helper.dll");
    if (dll_path_w.empty()) {
        std::printf("GetModuleFileNameW failed: %lu\n", ::GetLastError());
        return 1;
    }

    const std::wstring log_path_w = ReplaceFilenameW(exe_path_w, L"alpharom_helper.log");
    ::DeleteFileW(log_path_w.c_str());

    std::printf("[*] EXE: %s\n", WideToUtf8(exe_path_w).c_str());
    std::printf("[*] DLL: %s\n", WideToUtf8(dll_path_w).c_str());
    std::printf("[*] LOG: %s\n", WideToUtf8(log_path_w).c_str());
    std::printf("    (if you can't find the log, UAC may have redirected it to\n"
                "     %%LOCALAPPDATA%%\\VirtualStore\\... -- check there)\n");

    const std::wstring work_dir_w = ExtractParentDirW(exe_path_w);
    const wchar_t* work_dir_ptr = work_dir_w.empty() ? nullptr : work_dir_w.c_str();

    std::wstring cmd_line_w;
    cmd_line_w.reserve(exe_path_w.size() + 2U);
    cmd_line_w.push_back(L'"');
    cmd_line_w += exe_path_w;
    cmd_line_w.push_back(L'"');
    std::vector<wchar_t> cmd_buf(cmd_line_w.begin(), cmd_line_w.end());
    cmd_buf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(exe_path_w.c_str(), cmd_buf.data(), nullptr, nullptr, FALSE,
                          CREATE_SUSPENDED, nullptr, work_dir_ptr, &si, &pi)) {
        std::printf("CreateProcessW failed: %lu\n", ::GetLastError());
        return 1;
    }

    const ScopedHandle proc_guard(pi.hProcess);
    const ScopedHandle thread_guard(pi.hThread);
    ProcessTerminateGuard term_guard(pi.hProcess);

    std::printf("[*] PID=%lu (suspended)\n", pi.dwProcessId);

    const SIZE_T path_bytes = (dll_path_w.size() + 1U) * sizeof(wchar_t);
    LPVOID remote_buf = ::VirtualAllocEx(
        pi.hProcess, nullptr, path_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remote_buf == nullptr) {
        std::printf("VirtualAllocEx failed: %lu\n", ::GetLastError());
        return 2;
    }

    DWORD hmod = 0;
    {
        const RemoteMemoryGuard remote_mem_guard(pi.hProcess, remote_buf);

        SIZE_T written = 0;
        if (!::WriteProcessMemory(pi.hProcess, remote_buf, dll_path_w.c_str(),
                                  path_bytes, &written) ||
            written != path_bytes) {
            std::printf("WriteProcessMemory failed: %lu\n", ::GetLastError());
            return 2;
        }

        HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll");
        if (k32 == nullptr) {
            std::printf("GetModuleHandleW(kernel32.dll) failed: %lu\n", ::GetLastError());
            return 3;
        }
        FARPROC load_lib_w = ::GetProcAddress(k32, "LoadLibraryW");
        if (load_lib_w == nullptr) {
            std::printf("GetProcAddress(LoadLibraryW) failed: %lu\n", ::GetLastError());
            return 3;
        }
        std::printf("[*] LoadLibraryW = %p, remote DLL path = %p\n",
                    reinterpret_cast<void*>(load_lib_w), remote_buf);

        const ScopedHandle remote_thread(::CreateRemoteThread(
            pi.hProcess, nullptr, 0U,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(load_lib_w),
            remote_buf, 0U, nullptr));
        if (remote_thread.get() == nullptr) {
            std::printf("CreateRemoteThread failed: %lu\n", ::GetLastError());
            return 3;
        }

        ::WaitForSingleObject(remote_thread.get(), INFINITE);
        ::GetExitCodeThread(remote_thread.get(), &hmod);
    }

    std::printf("[*] LoadLibraryW returned 0x%08lX (HMODULE)\n", hmod);
    if (hmod == 0) {
        std::printf("[!] DLL failed to load.\n");
        return 4;
    }

    term_guard.Release();
    std::printf("[*] Resuming target main thread...\n");
    ::ResumeThread(pi.hThread);

    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 0;
    ::GetExitCodeProcess(pi.hProcess, &exit_code);
    std::printf("[*] Process exited with code 0x%08lX\n", exit_code);

    std::printf("\n[alpharom_helper.log]\n");
    FILE* raw_file = nullptr;
    if (::_wfopen_s(&raw_file, log_path_w.c_str(), L"rb") != 0 || raw_file == nullptr) {
        std::printf("(no log file)\n");
        return 0;
    }
    const std::unique_ptr<FILE, decltype(&std::fclose)> file_guard(raw_file, &std::fclose);

    char buf[kLogReadChunkBytes];
    for (;;) {
        const std::size_t n = std::fread(buf, 1U, sizeof(buf) - 1U, file_guard.get());
        if (n == 0U) {
            break;
        }
        buf[n] = '\0';
        std::fputs(buf, stdout);
    }
    return 0;
}

}  // namespace

int main() {
    try {
        return RunLauncher();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Fatal error: %s\n", e.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "Unknown fatal error\n");
        return 1;
    }
}
