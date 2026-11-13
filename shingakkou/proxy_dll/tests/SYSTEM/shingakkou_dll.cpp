// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// SYSTEM -- End-to-end black-box test for the built proxy DLL.
//
// Strategy: build a tiny "driver" EXE that LoadLibraryW's the proxy
// (driver_exe.cpp, in the same directory). This gtest spawns the
// driver, waits for it to exit, then reads the log file the proxy
// wrote during DllMain and asserts the game-specific banner is in
// there.
//
// What this catches (that DONT_RESOLVE_DLL_REFERENCES inspection
// could not):
//   - DllMain crashes / returns FALSE
//   - Log subsystem is broken (file never opens, flush missing)
//   - Wrong proxy got built/linked (banner mismatch)
//   - ProxyInit fails before banner is written
//
// All four compile-time #defines are set by the SYSTEM CMakeLists:
//   PROXY_DLL_PATH    -- absolute path to the built proxy DLL
//   DRIVER_EXE_PATH   -- absolute path to the driver_exe
//   LOG_FILE_PATH     -- where the proxy writes its log (relative or
//                        absolute; if relative, must resolve against
//                        the gtest's WORKING_DIRECTORY, which the
//                        driver inherits via CreateProcess)
//   EXPECTED_BANNER   -- substring that must appear in the log

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifndef PROXY_DLL_PATH
#define PROXY_DLL_PATH ""
#endif
#ifndef DRIVER_EXE_PATH
#define DRIVER_EXE_PATH ""
#endif
#ifndef LOG_FILE_PATH
#define LOG_FILE_PATH ""
#endif
#ifndef EXPECTED_BANNER
#define EXPECTED_BANNER ""
#endif

namespace {

constexpr DWORD kDriverWaitTimeoutMs = 30000u;
constexpr UINT kTimeoutTerminateExitCode = 99u;
constexpr DWORD kSentinelExitCode = 0xDEADBEEFu;
constexpr std::size_t kBackslashEscapeFactor = 2u;

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE h) noexcept : handle_(h) {}
    ~ScopedHandle() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    HANDLE get() const noexcept { return handle_; }

private:
    HANDLE handle_ = nullptr;
};

bool Utf8ToWide(std::string_view utf8, std::wstring& out) {
    out.clear();
    if (utf8.empty()) {
        return false;
    }
    const int needed = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
        static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        return false;
    }
    out.resize(static_cast<std::size_t>(needed));
    const int written = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
        static_cast<int>(utf8.size()), out.data(), needed);
    return written == needed;
}

std::wstring QuoteWindowsCommandArgW(std::wstring_view arg) {
    std::wstring out;
    out.push_back(L'"');
    std::size_t backslashes = 0;
    for (const wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            out.append(backslashes * kBackslashEscapeFactor + 1u, L'\\');
            out.push_back(L'"');
            backslashes = 0;
            continue;
        }
        out.append(backslashes, L'\\');
        out.push_back(ch);
        backslashes = 0;
    }
    out.append(backslashes * kBackslashEscapeFactor, L'\\');
    out.push_back(L'"');
    return out;
}

std::string ReadAllBytes(std::string_view utf8_path) {
    std::ifstream f(std::filesystem::u8path(utf8_path), std::ios::binary);
    if (!f) {
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Spawn the driver EXE with the proxy DLL path as argv[1]. Wait up
// to 30s for exit (DllMain spawns a polling thread in some games
// that observes g_shutdownRequested and bails within ms of
// FreeLibrary; 30s is a generous ceiling on top of that).
bool RunDriverExe(std::string_view exe_path, std::string_view dll_arg,
                  DWORD* out_exit_code) {
    std::wstring exe_w;
    std::wstring dll_w;
    if (!Utf8ToWide(exe_path, exe_w) || !Utf8ToWide(dll_arg, dll_w)) {
        return false;
    }

    const std::wstring cmd_line =
        L"\"" + exe_w + L"\" " + QuoteWindowsCommandArgW(dll_w);
    std::vector<wchar_t> cmd_buf(cmd_line.begin(), cmd_line.end());
    cmd_buf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe_w.c_str(), cmd_buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    ScopedHandle process_handle(pi.hProcess);
    ScopedHandle thread_handle(pi.hThread);

    const DWORD wait_rc = WaitForSingleObject(process_handle.get(), kDriverWaitTimeoutMs);
    if (wait_rc != WAIT_OBJECT_0) {
        TerminateProcess(process_handle.get(), kTimeoutTerminateExitCode);
        return false;
    }
    GetExitCodeProcess(process_handle.get(), out_exit_code);
    return true;
}

}  // namespace

TEST(ProxyDllSystem, DriverLoadsDllAndDllMainWritesBanner) {
    ASSERT_STRNE(PROXY_DLL_PATH, "")
        << "PROXY_DLL_PATH must be set at build time";
    ASSERT_STRNE(DRIVER_EXE_PATH, "")
        << "DRIVER_EXE_PATH must be set at build time";
    ASSERT_STRNE(LOG_FILE_PATH, "")
        << "LOG_FILE_PATH must be set at build time";
    ASSERT_STRNE(EXPECTED_BANNER, "")
        << "EXPECTED_BANNER must be set at build time";

    // Remove any leftover log from a previous test run so we are
    // certain the bytes we read came from THIS driver invocation.
    std::error_code ec;
    std::filesystem::remove(std::filesystem::u8path(LOG_FILE_PATH), ec);

    DWORD exit_code = kSentinelExitCode;
    ASSERT_TRUE(RunDriverExe(DRIVER_EXE_PATH, PROXY_DLL_PATH, &exit_code))
        << "driver EXE failed to spawn or timed out -- DllMain may "
           "have hung, or CreateProcessW failed (gle="
        << GetLastError() << ")";
    // exit_code is INFORMATIONAL only. Some proxies (Adieu, SakuraNoUta)
    // install a hook in DllMain that reads a fixed game RVA; in our
    // non-game host that page is unmapped, the read AVs inside DllMain,
    // and LoadLibraryW returns NULL with gle=1114. The banner is still
    // on disk (Log() fflushes each line), which is what the test cares
    // about. We surface exit_code in the message stream so a real
    // regression (early DllMain crash before banner) is debuggable.
    std::cout << "[ NOTE     ] driver exit_code=" << exit_code
              << " (0 = clean ATTACH; non-zero usually = post-banner DllMain hook crash)\n";

    const std::string log = ReadAllBytes(LOG_FILE_PATH);
    ASSERT_FALSE(log.empty())
        << "log file empty or missing: " << LOG_FILE_PATH
        << " -- did DllMain run and reach LogInit/log_init?";

    EXPECT_NE(log.find(EXPECTED_BANNER), std::string::npos)
        << "log did not contain expected banner.\n"
        << "  expected substring: " << EXPECTED_BANNER << "\n"
        << "  log path:           " << LOG_FILE_PATH << "\n"
        << "  log contents:\n" << log;
}
