// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// End-to-end black-box system tests for the built 32-bit EXTRAVAGANZA_CE
// winmm.dll proxy: spawns extravaganza_ce_driver.exe inside isolated RAII
// temporary directories to verify both DllMain startup banner logging and
// automatic clean-install asset patching on process attach.
#include "menu_autopatch.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
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

namespace fs = std::filesystem;

namespace {

using Bytes = std::vector<std::uint8_t>;

constexpr DWORD kDriverWaitTimeoutMs = 30000u;
constexpr UINT kTimeoutTerminateExitCode = 99u;
constexpr DWORD kSentinelExitCode = 0xDEADBEEFu;
constexpr std::size_t kBackslashEscapeFactor = 2u;

constexpr std::size_t kPatchMagicBytes = 8u;
constexpr char kPatchMagic[kPatchMagicBytes] = {'E', 'X', 'C', 'E', 'P', 'A', 'T', '1'};
constexpr std::uint32_t kPatchVersion = 1u;
constexpr std::uint32_t kStandaloneCount = 1u;
constexpr std::uint32_t kArchiveCount = 1u;
constexpr std::uint32_t kModifiedGpkCount = 1u;
constexpr std::uint32_t kLowByteMask = 0xFFu;
constexpr unsigned int kShift8 = 8u;
constexpr unsigned int kShift16 = 16u;
constexpr unsigned int kShift24 = 24u;

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

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view prefix) {
        static std::atomic<std::uint64_t> counter{0};
        const std::uint64_t id = counter.fetch_add(1u, std::memory_order_relaxed);
        const std::string dir_name =
            std::string(prefix) + "_" + std::to_string(GetCurrentProcessId()) +
            "_" + std::to_string(id);
        path_ = fs::temp_directory_path() / fs::u8path(dir_name);
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

void AppendU16Le(Bytes& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & kLowByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kShift8) & kLowByteMask));
}

void AppendU32Le(Bytes& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & kLowByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kShift8) & kLowByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kShift16) & kLowByteMask));
    out.push_back(static_cast<std::uint8_t>((v >> kShift24) & kLowByteMask));
}

void WriteBytes(const fs::path& path, const Bytes& data) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(ofs.good());
    if (!data.empty()) {
        ofs.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }
    ASSERT_TRUE(ofs.good());
}

Bytes ReadBytes(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs) {
        return {};
    }
    const std::streamsize sz = ifs.tellg();
    ifs.seekg(0, std::ios::beg);
    Bytes buf(static_cast<std::size_t>(sz));
    if (sz > 0) {
        ifs.read(reinterpret_cast<char*>(buf.data()), sz);
    }
    return buf;
}

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

std::string ReadAllText(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return {};
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool RunDriverExeInDir(std::string_view exe_path,
                       const std::wstring& dll_arg_w,
                       const std::wstring& work_dir_w,
                       DWORD* out_exit_code) {
    std::wstring exe_w;
    if (!Utf8ToWide(exe_path, exe_w)) {
        return false;
    }

    const std::wstring cmd_line =
        L"\"" + exe_w + L"\" " + QuoteWindowsCommandArgW(dll_arg_w);
    std::vector<wchar_t> cmd_buf(cmd_line.begin(), cmd_line.end());
    cmd_buf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe_w.c_str(), cmd_buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, work_dir_w.c_str(), &si, &pi)) {
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
    ASSERT_STRNE(PROXY_DLL_PATH, "");
    ASSERT_STRNE(DRIVER_EXE_PATH, "");
    ASSERT_STRNE(LOG_FILE_PATH, "");
    ASSERT_STRNE(EXPECTED_BANNER, "");

    ScopedTempDir work_dir("exc_proxy_st_banner");
    std::wstring dll_w;
    ASSERT_TRUE(Utf8ToWide(PROXY_DLL_PATH, dll_w));

    DWORD exit_code = kSentinelExitCode;
    ASSERT_TRUE(RunDriverExeInDir(
        DRIVER_EXE_PATH, dll_w, work_dir.path().wstring(), &exit_code))
        << "driver EXE failed to spawn or timed out (gle=" << GetLastError() << ")";
    EXPECT_EQ(exit_code, 0u);

    const fs::path log_path = work_dir.path() / fs::u8path(LOG_FILE_PATH);
    const std::string log = ReadAllText(log_path);
    ASSERT_FALSE(log.empty()) << "log file empty or missing: " << log_path.u8string();
    EXPECT_NE(log.find(EXPECTED_BANNER), std::string::npos) << log;
}

TEST(ProxyDllSystem, CompiledDllAutoPatchesCleanInstallOnProcessAttach) {
    ASSERT_STRNE(PROXY_DLL_PATH, "");
    ASSERT_STRNE(DRIVER_EXE_PATH, "");

    ScopedTempDir sandbox("exc_proxy_st_autopatch");
    const fs::path sandbox_dll = sandbox.path() / "winmm.dll";
    std::error_code ec;
    fs::copy_file(fs::u8path(PROXY_DLL_PATH), sandbox_dll,
                  fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << "Failed copying winmm.dll into sandbox: " << ec.message();

    const std::string orig_spt_str = "蟲愛でる少女・原本スクリプト";
    const std::string new_spt_str = "The Girl Who Loved Insects - Patched Script";
    const Bytes orig_spt(orig_spt_str.begin(), orig_spt_str.end());
    const Bytes new_spt(new_spt_str.begin(), new_spt_str.end());

    const Bytes orig_gpk = {0x42u, 0x4Du, 0x10u, 0x20u, 0x30u, 0x40u};
    const Bytes new_gpk = {0x42u, 0x4Du, 0xAAu, 0xBBu, 0xCCu, 0xDDu, 0xEEu, 0xFFu};

    Bytes gtb;
    AppendU32Le(gtb, kArchiveCount);
    AppendU32Le(gtb, 0u);
    AppendU32Le(gtb, 0u);
    const std::string_view entry_name = "btn_start";
    gtb.insert(gtb.end(), entry_name.begin(), entry_name.end());
    gtb.push_back(0u);

    const std::uint32_t gtb_hash =
        menu_autopatch::ComputeMurmurHash2(gtb.data(), gtb.size());
    const std::uint32_t orig_spt_hash =
        menu_autopatch::ComputeMurmurHash2(orig_spt.data(), orig_spt.size());
    const std::uint32_t new_spt_hash =
        menu_autopatch::ComputeMurmurHash2(new_spt.data(), new_spt.size());
    const std::uint32_t orig_gpk_hash =
        menu_autopatch::ComputeMurmurHash2(orig_gpk.data(), orig_gpk.size());
    const std::uint32_t new_gpk_hash =
        menu_autopatch::ComputeMurmurHash2(new_gpk.data(), new_gpk.size());

    Bytes patch_bin;
    patch_bin.insert(patch_bin.end(), kPatchMagic, kPatchMagic + kPatchMagicBytes);
    AppendU32Le(patch_bin, kPatchVersion);
    AppendU32Le(patch_bin, kStandaloneCount);
    AppendU32Le(patch_bin, static_cast<std::uint32_t>(orig_gpk.size()));
    AppendU32Le(patch_bin, static_cast<std::uint32_t>(new_gpk.size()));
    AppendU32Le(patch_bin, static_cast<std::uint32_t>(gtb.size()));
    AppendU32Le(patch_bin, gtb_hash);
    AppendU32Le(patch_bin, static_cast<std::uint32_t>(gtb.size()));
    AppendU32Le(patch_bin, gtb_hash);
    AppendU32Le(patch_bin, kModifiedGpkCount);

    const std::string_view rel_spt = "spt\\01.spt";
    Bytes masked_spt = new_spt;
    menu_autopatch::XorMaskPayload(
        masked_spt.data(), masked_spt.size(), orig_spt.data(), orig_spt.size(), orig_spt_hash);
    AppendU16Le(patch_bin, static_cast<std::uint16_t>(rel_spt.size()));
    patch_bin.insert(patch_bin.end(), rel_spt.begin(), rel_spt.end());
    AppendU32Le(patch_bin, static_cast<std::uint32_t>(orig_spt.size()));
    AppendU32Le(patch_bin, orig_spt_hash);
    AppendU32Le(patch_bin, static_cast<std::uint32_t>(new_spt.size()));
    AppendU32Le(patch_bin, new_spt_hash);
    patch_bin.insert(patch_bin.end(), masked_spt.begin(), masked_spt.end());

    Bytes masked_gtb = gtb;
    menu_autopatch::XorMaskPayload(
        masked_gtb.data(), masked_gtb.size(), gtb.data(), gtb.size(), gtb_hash);
    patch_bin.insert(patch_bin.end(), masked_gtb.begin(), masked_gtb.end());

    Bytes masked_gpk = new_gpk;
    menu_autopatch::XorMaskPayload(
        masked_gpk.data(), masked_gpk.size(), orig_gpk.data(), orig_gpk.size(), orig_gpk_hash);
    AppendU32Le(patch_bin, 0u);
    AppendU32Le(patch_bin, 0u);
    AppendU32Le(patch_bin, static_cast<std::uint32_t>(orig_gpk.size()));
    AppendU32Le(patch_bin, orig_gpk_hash);
    AppendU32Le(patch_bin, static_cast<std::uint32_t>(new_gpk.size()));
    AppendU32Le(patch_bin, new_gpk_hash);
    patch_bin.insert(patch_bin.end(), masked_gpk.begin(), masked_gpk.end());

    WriteBytes(sandbox.path() / "spt" / "01.spt", orig_spt);
    WriteBytes(sandbox.path() / "dwq" / "sys0.gtb", gtb);
    WriteBytes(sandbox.path() / "dwq" / "sys0.gpk", orig_gpk);
    WriteBytes(sandbox.path() / "extravaganza_patch.bin", patch_bin);

    DWORD exit_code = kSentinelExitCode;
    ASSERT_TRUE(RunDriverExeInDir(
        DRIVER_EXE_PATH, sandbox_dll.wstring(), sandbox.path().wstring(), &exit_code));
    EXPECT_EQ(exit_code, 0u);

    EXPECT_TRUE(fs::is_regular_file(sandbox.path() / "extravaganza_patch.bin.applied"));
    EXPECT_EQ(ReadBytes(sandbox.path() / "spt" / "01.spt"), new_spt);
    EXPECT_EQ(ReadBytes(sandbox.path() / "spt" / "01.spt.bak"), orig_spt);
    EXPECT_EQ(ReadBytes(sandbox.path() / "dwq" / "sys0.gpk"), new_gpk);
    EXPECT_EQ(ReadBytes(sandbox.path() / "dwq" / "sys0.gpk.bak"), orig_gpk);

    const std::string log = ReadAllText(sandbox.path() / fs::u8path(LOG_FILE_PATH));
    EXPECT_NE(log.find("Auto-patched EXTRAVAGANZA_CE game assets"), std::string::npos) << log;
}
