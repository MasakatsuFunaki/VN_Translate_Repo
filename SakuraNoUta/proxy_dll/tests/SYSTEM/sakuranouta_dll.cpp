// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// End-to-end black-box test: spawn sakuranouta_driver via CreateProcessW to
// LoadLibraryW the proxy DLL, then assert the expected banner appears in the
// generated log file.

#include "menu_patch_format.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

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

namespace fs = std::filesystem;

constexpr DWORD kDriverWaitTimeoutMs = 30'000;
constexpr UINT kTimeoutTerminateExitCode = 99;
constexpr DWORD kInitialExitCodeSentinel = 0xDEADBEEFu;
constexpr std::size_t kEscapedBackslashMultiplier = 2;

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle) noexcept : handle_(handle) {}

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ~ScopedHandle() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    HANDLE get() const noexcept { return handle_; }

private:
    HANDLE handle_;
};

bool Utf8ToWideStrict(std::string_view utf8, std::wstring& out) {
    out.clear();
    if (utf8.empty()) {
        return true;
    }
    const int len = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
        static_cast<int>(utf8.size()), nullptr, 0);
    if (len <= 0) {
        return false;
    }
    std::wstring wide(static_cast<std::size_t>(len), L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
        static_cast<int>(utf8.size()), wide.data(), len);
    if (written != len) {
        return false;
    }
    out = std::move(wide);
    return true;
}

std::wstring QuoteWindowsArg(std::wstring_view arg) {
    if (!arg.empty() &&
        arg.find_first_of(L" \t\n\v\"") == std::wstring_view::npos) {
        return std::wstring(arg);
    }

    std::wstring quoted;
    quoted.push_back(L'"');
    std::size_t pending_backslashes = 0;
    for (const wchar_t ch : arg) {
        if (ch == L'\\') {
            ++pending_backslashes;
            continue;
        }
        if (ch == L'"') {
            quoted.append(
                pending_backslashes * kEscapedBackslashMultiplier + 1, L'\\');
            quoted.push_back(L'"');
            pending_backslashes = 0;
            continue;
        }
        quoted.append(pending_backslashes, L'\\');
        quoted.push_back(ch);
        pending_backslashes = 0;
    }
    quoted.append(pending_backslashes * kEscapedBackslashMultiplier, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::string ReadAllBytes(std::string_view utf8_path) {
    std::ifstream file(fs::u8path(utf8_path), std::ios::binary);
    if (!file) {
        return {};
    }
    std::stringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

bool RunDriverExe(std::string_view exe_utf8, std::string_view dll_utf8,
                  DWORD* out_exit_code) {
    if (exe_utf8.find('"') != std::string_view::npos) {
        return false;
    }
    std::wstring exe_w;
    std::wstring dll_w;
    if (!Utf8ToWideStrict(exe_utf8, exe_w) || exe_w.empty()) {
        return false;
    }
    if (!Utf8ToWideStrict(dll_utf8, dll_w) || dll_w.empty()) {
        return false;
    }
    std::wstring cmd_w = L"\"" + exe_w + L"\" " + QuoteWindowsArg(dll_w);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe_w.c_str(), cmd_w.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    const ScopedHandle process_handle(pi.hProcess);
    const ScopedHandle thread_handle(pi.hThread);

    const DWORD wait_rc =
        WaitForSingleObject(process_handle.get(), kDriverWaitTimeoutMs);
    if (wait_rc != WAIT_OBJECT_0) {
        TerminateProcess(process_handle.get(), kTimeoutTerminateExitCode);
        return false;
    }
    if (!GetExitCodeProcess(process_handle.get(), out_exit_code)) {
        return false;
    }
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

    std::wstring log_path_w;
    ASSERT_TRUE(Utf8ToWideStrict(LOG_FILE_PATH, log_path_w))
        << "LOG_FILE_PATH is not valid UTF-8: " << LOG_FILE_PATH;
    ASSERT_FALSE(log_path_w.empty());

    // Remove any leftover log from a previous test run so the bytes read
    // below come exclusively from this driver invocation.
    std::error_code ec;
    fs::remove(fs::u8path(LOG_FILE_PATH), ec);

    DWORD exit_code = kInitialExitCodeSentinel;
    ASSERT_TRUE(RunDriverExe(DRIVER_EXE_PATH, PROXY_DLL_PATH, &exit_code))
        << "driver EXE failed to spawn or timed out -- DllMain may "
           "have hung, or CreateProcessW failed (gle="
        << GetLastError() << ")";

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

namespace {

constexpr std::size_t kStOrigSliceBytes = 96u;
constexpr std::size_t kStNewPayloadBytes = 84u;
constexpr unsigned int kStBitsPerByte = 8u;
constexpr std::size_t kStU32Bytes = 4u;
constexpr std::uint32_t kStByteMask = 0xFFu;

class ScopedSystemSandbox {
public:
    ScopedSystemSandbox() {
        static std::atomic<unsigned int> seq{0u};
        path_ = fs::temp_directory_path() /
                fs::u8path("snu_proxy_st_" + std::to_string(++seq));
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }

    ~ScopedSystemSandbox() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    ScopedSystemSandbox(const ScopedSystemSandbox&) = delete;
    ScopedSystemSandbox& operator=(const ScopedSystemSandbox&) = delete;

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

void WriteVecFile(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(static_cast<bool>(out)) << path.u8string();
    if (!bytes.empty()) {
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    ASSERT_TRUE(static_cast<bool>(out)) << path.u8string();
}

std::vector<std::uint8_t> ReadVecFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return {};
    }
    const auto sz = static_cast<std::size_t>(in.tellg());
    std::vector<std::uint8_t> buf(sz, 0u);
    in.seekg(0);
    if (sz > 0u) {
        in.read(reinterpret_cast<char*>(buf.data()),
                static_cast<std::streamsize>(sz));
    }
    return buf;
}

std::vector<std::uint8_t> BuildSingleEntryArc20(
    std::string_view name,
    const std::array<std::uint8_t, menu_patch_format::kArc20ExtraFieldBytes>& extra,
    const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> arc;
    arc.insert(arc.end(), menu_patch_format::kArc20Magic,
               menu_patch_format::kArc20Magic + menu_patch_format::kArc20MagicBytes);
    auto append_le32 = [&](std::uint32_t v) {
        for (unsigned int i = 0u; i < kStU32Bytes; ++i) {
            arc.push_back(static_cast<std::uint8_t>(
                (v >> (i * kStBitsPerByte)) & kStByteMask));
        }
    };
    append_le32(1u);
    std::array<std::uint8_t, menu_patch_format::kArc20NameFieldBytes> name_slot{};
    std::memcpy(name_slot.data(), name.data(), name.size());
    arc.insert(arc.end(), name_slot.begin(), name_slot.end());
    append_le32(0u);
    append_le32(static_cast<std::uint32_t>(payload.size()));
    arc.insert(arc.end(), extra.begin(), extra.end());
    arc.insert(arc.end(), payload.begin(), payload.end());
    return arc;
}

}  // namespace

TEST(ProxyDllSystem,
     DriverAppliesStagedMenuPatchBinAndFastPathsSecondLaunchEndToEnd) {
    ScopedSystemSandbox sandbox;
    const fs::path staged_driver = sandbox.path() / "sakuranouta_driver.exe";
    const fs::path staged_dll = sandbox.path() / "winmm.dll";
    fs::copy_file(fs::u8path(DRIVER_EXE_PATH), staged_driver);
    fs::copy_file(fs::u8path(PROXY_DLL_PATH), staged_dll);

    std::array<std::uint8_t, menu_patch_format::kArc20ExtraFieldBytes> extra{};
    extra.fill(0x4Du);
    std::vector<std::uint8_t> orig_sysgrp_entry(kStOrigSliceBytes, 0u);
    std::vector<std::uint8_t> new_sysgrp_entry(kStNewPayloadBytes, 0u);
    std::vector<std::uint8_t> orig_att01(kStOrigSliceBytes, 0u);
    std::vector<std::uint8_t> new_att01(kStNewPayloadBytes, 0u);
    for (std::size_t i = 0u; i < kStOrigSliceBytes; ++i) {
        orig_sysgrp_entry[i] = static_cast<std::uint8_t>((i * 7u + 0x13u) & 0xFFu);
        orig_att01[i] = static_cast<std::uint8_t>((i * 11u + 0x29u) & 0xFFu);
    }
    for (std::size_t i = 0u; i < kStNewPayloadBytes; ++i) {
        new_sysgrp_entry[i] = static_cast<std::uint8_t>((i * 17u + 0x51u) & 0xFFu);
        new_att01[i] = static_cast<std::uint8_t>((i * 23u + 0x67u) & 0xFFu);
    }

    const std::vector<std::uint8_t> orig_sysgrp =
        BuildSingleEntryArc20("SGTitle990000", extra, orig_sysgrp_entry);
    const std::vector<std::uint8_t> expected_sysgrp =
        BuildSingleEntryArc20("SGTitle990000", extra, new_sysgrp_entry);
    const std::vector<std::uint8_t> orig_data02501 =
        BuildSingleEntryArc20("att01", extra, orig_att01);

    std::uint64_t sysgrp_off = 0u;
    std::uint64_t sysgrp_sz = 0u;
    std::uint64_t att01_off = 0u;
    std::uint64_t att01_sz = 0u;
    ASSERT_EQ(menu_patch_format::FindArc20EntrySlice(
                  menu_patch_format::ByteSpan(orig_sysgrp), "SGTitle990000",
                  sysgrp_off, sysgrp_sz),
              menu_patch_format::ArcStatus::kOk);
    ASSERT_EQ(menu_patch_format::FindArc20EntrySlice(
                  menu_patch_format::ByteSpan(orig_data02501), "att01",
                  att01_off, att01_sz),
              menu_patch_format::ArcStatus::kOk);

    std::vector<std::uint8_t> patch_blob;
    ASSERT_EQ(menu_patch_format::SerializeMenuPatchManifest(
                  {
                      {"sysgrp.arc", "sysgrp.arc", "SGTitle990000", sysgrp_off,
                       orig_sysgrp_entry, new_sysgrp_entry,
                       menu_patch_format::kFlagArchiveEntry},
                      {"data02501.arc", "att01", "att01", att01_off, orig_att01,
                       new_att01, menu_patch_format::kFlagBackupStandalone},
                  },
                  patch_blob),
              menu_patch_format::ManifestStatus::kOk);

    WriteVecFile(sandbox.path() / "sysgrp.arc", orig_sysgrp);
    WriteVecFile(sandbox.path() / "data02501.arc", orig_data02501);
    WriteVecFile(sandbox.path() / "menu_patch.bin", patch_blob);

    // Launch 1: compiled winmm.dll applies menu_patch.bin under DllMain.
    DWORD exit_code_1 = kInitialExitCodeSentinel;
    ASSERT_TRUE(RunDriverExe(staged_driver.u8string(), staged_dll.u8string(),
                             &exit_code_1));
    EXPECT_EQ(exit_code_1, 0u);

    const fs::path sandbox_log = sandbox.path() / "proxy_log.txt";
    const std::string log1 = ReadAllBytes(sandbox_log.u8string());
    EXPECT_NE(log1.find("[MenuAutoPatch] Outcome: kApplied"), std::string::npos)
        << log1;
    EXPECT_EQ(ReadVecFile(sandbox.path() / "sysgrp.arc"), expected_sysgrp);
    EXPECT_EQ(ReadVecFile(sandbox.path() / "sysgrp.arc.bak"), orig_sysgrp);
    EXPECT_EQ(ReadVecFile(sandbox.path() / "att01"), new_att01);
    EXPECT_TRUE(fs::is_regular_file(sandbox.path() / "menu_patch.applied"));

    // Launch 2: compiled winmm.dll hits sentinel fast-path (`kAlreadyApplied`).
    fs::remove(sandbox_log);
    DWORD exit_code_2 = kInitialExitCodeSentinel;
    ASSERT_TRUE(RunDriverExe(staged_driver.u8string(), staged_dll.u8string(),
                             &exit_code_2));
    EXPECT_EQ(exit_code_2, 0u);
    const std::string log2 = ReadAllBytes(sandbox_log.u8string());
    EXPECT_NE(log2.find("[MenuAutoPatch] Outcome: kAlreadyApplied"),
              std::string::npos)
        << log2;
}
