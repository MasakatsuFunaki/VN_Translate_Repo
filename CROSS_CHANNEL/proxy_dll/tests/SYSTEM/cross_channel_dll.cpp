// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// End-to-end black-box system test for the built CROSS_CHANNEL proxy DLL.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "menu_patch_format.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
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

namespace fs = std::filesystem;

constexpr DWORD kDriverWaitTimeoutMs = 30000u;
constexpr UINT kTimeoutTerminateCode = 99u;
constexpr DWORD kSentinelExitCode = 0xDEADBEEFu;
constexpr std::size_t kBackslashPairFactor = 2u;
constexpr std::size_t kQuoteEscapeBackslashCount = 1u;

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE h = nullptr) : h_(h) {}
    ~ScopedHandle() {
        if (valid()) {
            CloseHandle(h_);
        }
    }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    HANDLE get() const { return h_; }
    bool valid() const { return h_ != nullptr && h_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE h_;
};

class ScopedTempDir {
public:
    explicit ScopedTempDir(const char* prefix) {
        static std::atomic<unsigned int> seq{0};
        const std::string dir_name =
            std::string(prefix) + std::to_string(::GetCurrentProcessId()) + "_" +
            std::to_string(++seq);
        dir_ = fs::temp_directory_path() / fs::u8path(dir_name);
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_, ec);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const fs::path& path() const noexcept { return dir_; }

private:
    fs::path dir_;
};

class ScopedFileRemover {
public:
    explicit ScopedFileRemover(fs::path path) : path_(std::move(path)) {
        RemoveNow();
    }

    ~ScopedFileRemover() {
        RemoveNow();
    }

    ScopedFileRemover(const ScopedFileRemover&) = delete;
    ScopedFileRemover& operator=(const ScopedFileRemover&) = delete;

private:
    void RemoveNow() const noexcept {
        if (path_.empty()) {
            return;
        }
        std::error_code ec;
        fs::remove(path_, ec);
    }

    fs::path path_;
};

bool WidenUtf8(const char* utf8, std::wstring& out) {
    if (utf8 == nullptr || *utf8 == '\0') {
        out.clear();
        return false;
    }
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, nullptr, 0);
    if (needed <= 0) {
        return false;
    }
    out.assign(static_cast<std::size_t>(needed), L'\0');
    const int written =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, &out[0], needed);
    if (written != needed) {
        out.clear();
        return false;
    }
    if (!out.empty() && out.back() == L'\0') {
        out.pop_back();
    }
    return !out.empty();
}

std::wstring QuoteCommandLineArgW(const std::wstring& arg) {
    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            quoted.append(backslashes * kBackslashPairFactor + kQuoteEscapeBackslashCount, L'\\');
            quoted.push_back(L'"');
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(ch);
    }
    quoted.append(backslashes * kBackslashPairFactor, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::string ReadAllBytes(const fs::path& path) {
    if (path.empty()) {
        return {};
    }
    try {
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            return {};
        }
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    } catch (const std::exception&) {
        return {};
    }
}

bool RunDriverExe(const char* exe_path,
                  const char* dll_arg,
                  const fs::path& work_dir,
                  DWORD* out_exit_code,
                  const std::wstring& extra_args = {}) {
    std::wstring exe_w;
    std::wstring dll_w;
    if (!WidenUtf8(exe_path, exe_w) || !WidenUtf8(dll_arg, dll_w)) {
        return false;
    }

    if (!work_dir.empty()) {
        const fs::path src_exe(exe_w);
        const fs::path staged_exe = work_dir / src_exe.filename();
        std::error_code ec;
        if (fs::copy_file(src_exe, staged_exe,
                          fs::copy_options::overwrite_existing, ec)) {
            exe_w = staged_exe.wstring();
        }
    }

    std::wstring cmd =
        L"\"" + exe_w + L"\" " + QuoteCommandLineArgW(dll_w);
    if (!extra_args.empty()) {
        cmd += L" " + extra_args;
    }
    std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back(L'\0');

    const std::wstring work_dir_w = work_dir.wstring();
    const wchar_t* work_dir_ptr = work_dir_w.empty() ? nullptr : work_dir_w.c_str();

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe_w.c_str(), cmd_buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, work_dir_ptr, &si, &pi)) {
        return false;
    }
    ScopedHandle proc_guard(pi.hProcess);
    ScopedHandle thread_guard(pi.hThread);

    const DWORD wait_rc = WaitForSingleObject(proc_guard.get(), kDriverWaitTimeoutMs);
    if (wait_rc != WAIT_OBJECT_0) {
        TerminateProcess(proc_guard.get(), kTimeoutTerminateCode);
        return false;
    }
    return GetExitCodeProcess(proc_guard.get(), out_exit_code) != FALSE;
}

using Bytes = std::vector<std::uint8_t>;

void AppendBe16(Bytes& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
}
void AppendBe32(Bytes& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
}
void AppendBe64(Bytes& out, std::uint64_t v) {
    AppendBe32(out, static_cast<std::uint32_t>((v >> 32) & 0xFFFFFFFFu));
    AppendBe32(out, static_cast<std::uint32_t>(v & 0xFFFFFFFFu));
}
void AppendLe32(Bytes& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
}

struct UtfCol {
    std::uint8_t flag;
    std::string name;
};

Bytes BuildUtfTable(const std::string& tname,
                    const std::vector<UtfCol>& cols,
                    const std::vector<Bytes>& rows,
                    const Bytes& blobs = {}) {
    Bytes strs;
    auto add_s = [&](const std::string& s) -> std::uint32_t {
        const auto off = static_cast<std::uint32_t>(strs.size());
        strs.insert(strs.end(), s.begin(), s.end());
        strs.push_back(0);
        return off;
    };
    const std::uint32_t tname_off = add_s(tname);
    Bytes schema;
    for (const auto& c : cols) {
        schema.push_back(c.flag);
        AppendBe32(schema, add_s(c.name));
    }
    const std::uint16_t row_len =
        rows.empty() ? 0u : static_cast<std::uint16_t>(rows[0].size());
    Bytes row_buf;
    for (const auto& r : rows) {
        row_buf.insert(row_buf.end(), r.begin(), r.end());
    }
    const auto rows_off = static_cast<std::uint32_t>(24u + schema.size());
    const auto strs_off = rows_off + static_cast<std::uint32_t>(row_buf.size());
    const auto data_off = strs_off + static_cast<std::uint32_t>(strs.size());
    const auto tsize = data_off + static_cast<std::uint32_t>(blobs.size());

    Bytes out = {'@', 'U', 'T', 'F'};
    AppendBe32(out, tsize);
    AppendBe32(out, rows_off);
    AppendBe32(out, strs_off);
    AppendBe32(out, data_off);
    AppendBe32(out, tname_off);
    AppendBe16(out, static_cast<std::uint16_t>(cols.size()));
    AppendBe16(out, row_len);
    AppendBe32(out, static_cast<std::uint32_t>(rows.size()));
    out.insert(out.end(), schema.begin(), schema.end());
    out.insert(out.end(), row_buf.begin(), row_buf.end());
    out.insert(out.end(), strs.begin(), strs.end());
    out.insert(out.end(), blobs.begin(), blobs.end());
    return out;
}

Bytes BuildMinimalSysCpk(const Bytes& sub0_payload) {
    Bytes pak;
    AppendLe32(pak, 16u);
    AppendLe32(pak, static_cast<std::uint32_t>(sub0_payload.size()));
    AppendLe32(pak, 0u);
    AppendLe32(pak, 0u);
    pak.insert(pak.end(), sub0_payload.begin(), sub0_payload.end());

    Bytes comp;
    menu_patch::LzssCompressZeroRing(pak, comp);
    const auto sz = static_cast<std::uint32_t>(comp.size());

    Bytes row_l;
    AppendBe16(row_l, 0u);
    AppendBe16(row_l, static_cast<std::uint16_t>(sz));
    AppendBe16(row_l, static_cast<std::uint16_t>(sz));
    const Bytes dl = BuildUtfTable(
        "CpkItocL", {{0x52, "ID"}, {0x52, "FileSize"}, {0x52, "ExtractSize"}}, {row_l});
    const Bytes dh = BuildUtfTable(
        "CpkItocH", {{0x52, "ID"}, {0x54, "FileSize"}, {0x54, "ExtractSize"}}, {});
    Bytes blobs = dl;
    const auto dh_off = static_cast<std::uint32_t>(blobs.size());
    blobs.insert(blobs.end(), dh.begin(), dh.end());

    Bytes ir;
    AppendBe32(ir, 1u);
    AppendBe32(ir, 0u);
    AppendBe32(ir, 0u);
    AppendBe32(ir, static_cast<std::uint32_t>(dl.size()));
    AppendBe32(ir, dh_off);
    AppendBe32(ir, static_cast<std::uint32_t>(dh.size()));
    const Bytes itoc_utf = BuildUtfTable(
        "CpkItocInfo",
        {{0x54, "FilesL"}, {0x54, "FilesH"}, {0x5B, "DataL"}, {0x5B, "DataH"}},
        {ir},
        blobs);

    const std::uint64_t csize = (static_cast<std::uint64_t>(sz) + 0x7FFu) & ~0x7FFull;
    Bytes hr;
    AppendBe64(hr, 0x1000u);
    AppendBe64(hr, csize);
    AppendBe64(hr, 0x800u);
    AppendBe64(hr, 16u + itoc_utf.size());
    AppendBe64(hr, sz);
    AppendBe64(hr, sz);
    AppendBe32(hr, 1u);
    AppendBe16(hr, 0x800u);
    const Bytes cpk_utf = BuildUtfTable(
        "CpkHeader",
        {{0x56, "ContentOffset"},
         {0x56, "ContentSize"},
         {0x56, "ItocOffset"},
         {0x56, "ItocSize"},
         {0x56, "EnabledPackedSize"},
         {0x56, "EnabledDataSize"},
         {0x54, "Files"},
         {0x52, "Align"}},
        {hr});

    Bytes cpk(0x1000u, 0u);
    std::memcpy(cpk.data(), "CPK ", 4);
    cpk[4] = 0xFFu;
    cpk[8] = static_cast<std::uint8_t>(cpk_utf.size() & 0xFFu);
    cpk[9] = static_cast<std::uint8_t>((cpk_utf.size() >> 8) & 0xFFu);
    std::memcpy(cpk.data() + 16, cpk_utf.data(), cpk_utf.size());

    std::memcpy(cpk.data() + 0x800, "ITOC", 4);
    cpk[0x804] = 0xFFu;
    cpk[0x808] = static_cast<std::uint8_t>(itoc_utf.size() & 0xFFu);
    cpk[0x809] = static_cast<std::uint8_t>((itoc_utf.size() >> 8) & 0xFFu);
    std::memcpy(cpk.data() + 0x810, itoc_utf.data(), itoc_utf.size());

    cpk.insert(cpk.end(), comp.begin(), comp.end());
    return cpk;
}

bool WriteBinaryFile(const fs::path& p, const Bytes& data) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    if (!data.empty()) {
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }
    out.flush();
    return static_cast<bool>(out);
}

Bytes ReadBinaryVector(const fs::path& p) {
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in) {
        return {};
    }
    const auto sz = static_cast<std::size_t>(in.tellg());
    in.seekg(0);
    Bytes buf(sz);
    if (sz > 0) {
        in.read(reinterpret_cast<char*>(buf.data()),
                static_cast<std::streamsize>(sz));
    }
    return buf;
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

    const ScopedTempDir temp_dir("crc_proxy_dll_st_");
    const fs::path configured_log = fs::u8path(LOG_FILE_PATH);
    const ScopedFileRemover cwd_log_guard(configured_log);
    const fs::path active_log_path =
        configured_log.is_absolute() ? configured_log : (temp_dir.path() / configured_log);
    const ScopedFileRemover active_log_guard(active_log_path);

    DWORD exit_code = kSentinelExitCode;
    ASSERT_TRUE(RunDriverExe(DRIVER_EXE_PATH, PROXY_DLL_PATH, temp_dir.path(), &exit_code))
        << "driver EXE failed to spawn or timed out (gle=" << GetLastError() << ")";
    EXPECT_EQ(exit_code, 0u);

    std::cout << "[ NOTE     ] driver exit_code=" << exit_code << "\n";

    const std::string log = ReadAllBytes(active_log_path);
    ASSERT_FALSE(log.empty())
        << "log file empty or missing: " << active_log_path.u8string();

    EXPECT_NE(log.find(EXPECTED_BANNER), std::string::npos)
        << "log did not contain expected banner.\n"
        << "  expected substring: " << EXPECTED_BANNER << "\n"
        << "  log path:           " << active_log_path.u8string() << "\n"
        << "  log contents:\n" << log;
}

TEST(ProxyDllSystem, AutoPatchesSysCpkAndLocalizesWin32DialogEndToEnd) {
    ASSERT_STRNE(PROXY_DLL_PATH, "");
    ASSERT_STRNE(DRIVER_EXE_PATH, "");

    const ScopedTempDir temp_dir("crc_proxy_e2e_st_");

    // Prepare a valid Criware sys.cpk + CCMP v1 menu_patch.bin inside the
    // hermetic temp game directory (never touching real user/game files).
    Bytes orig_sub(1024u);
    for (std::size_t i = 0; i < orig_sub.size(); ++i) {
        orig_sub[i] = static_cast<std::uint8_t>((i * 29u + 5u) & 0xFFu);
    }
    Bytes en_sub = orig_sub;
    std::fill_n(en_sub.begin() + 32, 64, static_cast<std::uint8_t>(0xCCu));

    const Bytes orig_cpk = BuildMinimalSysCpk(orig_sub);
    Bytes patch_blob;
    Bytes expected_patched_cpk;
    ASSERT_TRUE(menu_patch::MenuPatchPackage::BuildPatchBlob(
        orig_cpk, {{0u, 0u, orig_sub, en_sub}}, patch_blob, &expected_patched_cpk));

    const fs::path cpk_path = temp_dir.path() / "data" / "sys.cpk";
    const fs::path bak_path = temp_dir.path() / "data" / "sys.cpk.bak";
    const fs::path sentinel_path = temp_dir.path() / "data" / "sys.cpk.applied";
    const fs::path patch_path = temp_dir.path() / "menu_patch.bin";
    const fs::path probe_out = temp_dir.path() / "chrome_probe.txt";
    const fs::path log_path = temp_dir.path() / "proxy_log.txt";

    ASSERT_TRUE(WriteBinaryFile(cpk_path, orig_cpk));
    ASSERT_TRUE(WriteBinaryFile(patch_path, patch_blob));

    // Launch 1: real child process loads xinput1_3.dll -> TranslatorInit()
    // auto-patches data\sys.cpk + installs Win32 hooks -> dialog probe verifies
    // live WH_CALLWNDPROCRET WM_INITDIALOG localization and 9pt Segoe UI font.
    DWORD exit_code_1 = kSentinelExitCode;
    const std::wstring probe_args =
        L"--probe-chrome " + QuoteCommandLineArgW(probe_out.wstring());
    ASSERT_TRUE(RunDriverExe(DRIVER_EXE_PATH, PROXY_DLL_PATH, temp_dir.path(),
                             &exit_code_1, probe_args));
    ASSERT_EQ(exit_code_1, 0u);

    EXPECT_TRUE(fs::exists(bak_path));
    EXPECT_TRUE(fs::exists(sentinel_path));
    EXPECT_EQ(ReadBinaryVector(bak_path), orig_cpk);
    EXPECT_EQ(ReadBinaryVector(cpk_path), expected_patched_cpk);

    const std::string log1 = ReadAllBytes(log_path);
    EXPECT_NE(log1.find("MenuAutoPatch: patched data\\sys.cpk"), std::string::npos)
        << log1;
    EXPECT_NE(log1.find("WIN32-CHROME: installed 11/11 user32 hooks"),
              std::string::npos)
        << log1;
    EXPECT_NE(log1.find("Translator: seeded "), std::string::npos) << log1;

    const std::string probe = ReadAllBytes(probe_out);
    EXPECT_NE(probe.find("caption=About CROSS†CHANNEL\n"), std::string::npos)
        << probe;
    EXPECT_NE(probe.find("version=CROSS†CHANNEL Version 1.00\n"),
              std::string::npos)
        << probe;
    EXPECT_NE(probe.find("ok=OK\n"), std::string::npos) << probe;
    EXPECT_NE(probe.find("font_face=Segoe UI\n"), std::string::npos) << probe;
    EXPECT_NE(probe.find("font_height=-12\n"), std::string::npos) << probe;

    // Launch 2: hits <1 ms .applied fast-path without re-applying patch.
    DWORD exit_code_2 = kSentinelExitCode;
    ASSERT_TRUE(RunDriverExe(DRIVER_EXE_PATH, PROXY_DLL_PATH, temp_dir.path(),
                             &exit_code_2));
    ASSERT_EQ(exit_code_2, 0u);
    EXPECT_EQ(ReadBinaryVector(bak_path), orig_cpk);
    EXPECT_EQ(ReadBinaryVector(cpk_path), expected_patched_cpk);
    const std::string log2 = ReadAllBytes(log_path);
    EXPECT_EQ(log2.find("MenuAutoPatch: patched data\\sys.cpk"), std::string::npos)
        << "Second launch should hit .applied fast-path without rewriting sys.cpk:\n"
        << log2;
}
