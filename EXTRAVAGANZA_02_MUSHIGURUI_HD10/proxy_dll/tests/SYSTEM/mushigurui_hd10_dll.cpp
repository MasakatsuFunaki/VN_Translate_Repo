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

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifndef PROXY_DLL_PATH
#  define PROXY_DLL_PATH ""
#endif
#ifndef DRIVER_EXE_PATH
#  define DRIVER_EXE_PATH ""
#endif
#ifndef LOG_FILE_PATH
#  define LOG_FILE_PATH ""
#endif
#ifndef EXPECTED_BANNER
#  define EXPECTED_BANNER ""
#endif

namespace {

std::string ReadAllBytes(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Spawn the driver EXE with the proxy DLL path as argv[1]. Wait up
// to 30s for exit (DllMain spawns a polling thread in some games
// that observes g_shutdownRequested and bails within ms of
// FreeLibrary; 30s is a generous ceiling on top of that).
bool RunDriverExe(const char* exe_path, const char* dll_arg,
                  DWORD* out_exit_code) {
    std::string cmd = std::string("\"") + exe_path + "\" \"" + dll_arg + "\"";
    char buf[2048];
    std::strncpy(buf, cmd.c_str(), sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    STARTUPINFOA si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(nullptr, buf, nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    DWORD wait_rc = WaitForSingleObject(pi.hProcess, 30000);
    if (wait_rc != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 99);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return false;
    }
    GetExitCodeProcess(pi.hProcess, out_exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
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
    DeleteFileA(LOG_FILE_PATH);

    DWORD exit_code = 0xDEADBEEF;
    ASSERT_TRUE(RunDriverExe(DRIVER_EXE_PATH, PROXY_DLL_PATH, &exit_code))
        << "driver EXE failed to spawn or timed out -- DllMain may "
           "have hung, or CreateProcess failed (gle="
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

    std::string log = ReadAllBytes(LOG_FILE_PATH);
    ASSERT_FALSE(log.empty())
        << "log file empty or missing: " << LOG_FILE_PATH
        << " -- did DllMain run and reach LogInit/log_init?";

    EXPECT_NE(log.find(EXPECTED_BANNER), std::string::npos)
        << "log did not contain expected banner.\n"
        << "  expected substring: " << EXPECTED_BANNER << "\n"
        << "  log path:           " << LOG_FILE_PATH << "\n"
        << "  ---- log contents ----\n" << log;
}
