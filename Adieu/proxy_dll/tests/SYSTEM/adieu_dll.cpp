// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// End-to-end black-box system tests for Adieu's winmm.dll proxy:
//   1. Spawns adieu_driver.exe loading winmm.dll in an isolated sandbox with a
//      synthetic translation_table.tsv, verifying clean exit, banner + table
//      diagnostics in proxy_log.txt, safe prologue-mismatch skip on non-Siglus
//      hosts, and default suppression of debug capture/key dump files.
//   2. When VN_GAME_DIR is set, inspects retail SiglusEngine.exe read-only on
//      disk to verify exact PE .text prologues and stack-cleanup epilogues for
//      RVA 0x10C3A0, 0x14C8E0, and 0x14C9C0.

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
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
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
constexpr DWORD kCleanExitCode = 0u;
constexpr std::size_t kBackslashPairFactor = 2u;
constexpr std::size_t kQuoteEscapeBackslashCount = 1u;

constexpr uint32_t kWstringAssignRangeRva = 0x0010C3A0u;
constexpr uint32_t kWstringAssignRetOffset = 0x000000CCu;
constexpr uint32_t kSceneGetStrRva = 0x0014C8E0u;
constexpr uint32_t kSceneGetStrCallAssignOffset = 0x000000BAu;
constexpr uint32_t kSceneGetStrRetOffset = 0x000000DBu;
constexpr uint32_t kSceneGetCurStrRva = 0x0014C9C0u;
constexpr uint32_t kSceneGetCurStrSsV2XorOffset = 0x00000076u;

constexpr uint8_t kExpectedSehPrologue[5] = {
    0x55u, 0x8Bu, 0xECu, 0x6Au, 0xFFu,
};
constexpr uint8_t kExpectedRet0C[3] = {
    0xC2u, 0x0Cu, 0x00u,
};
constexpr uint8_t kExpectedSsV2Imul[7] = {
    0x69u, 0x45u, 0x0Cu, 0x87u, 0x70u, 0x00u, 0x00u,
};
constexpr uint8_t kCallRel32Opcode = 0xE8u;

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE h = nullptr) : h_(h) {}
    ~ScopedHandle() {
        if (valid()) {
            ::CloseHandle(h_);
        }
    }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    HANDLE get() const noexcept { return h_; }
    bool valid() const noexcept {
        return h_ != nullptr && h_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE h_;
};

class ScopedTempDir {
public:
    explicit ScopedTempDir(const char* prefix) {
        static std::atomic<unsigned int> seq{0u};
        const std::string dir_name =
            std::string(prefix) + std::to_string(::GetCurrentProcessId()) +
            "_" + std::to_string(++seq);
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

    ~ScopedFileRemover() { RemoveNow(); }

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
    const int needed = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                             utf8, -1, nullptr, 0);
    if (needed <= 0) {
        return false;
    }
    out.assign(static_cast<std::size_t>(needed), L'\0');
    const int written = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, out.data(), needed);
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
    std::size_t backslashes = 0u;
    for (const wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            quoted.append(backslashes * kBackslashPairFactor +
                              kQuoteEscapeBackslashCount,
                          L'\\');
            quoted.push_back(L'"');
            backslashes = 0u;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0u;
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
                  DWORD* out_exit_code) {
    std::wstring exe_w;
    std::wstring dll_w;
    if (!WidenUtf8(exe_path, exe_w) || !WidenUtf8(dll_arg, dll_w)) {
        return false;
    }

    const std::wstring cmd =
        L"\"" + exe_w + L"\" " + QuoteCommandLineArgW(dll_w);
    std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back(L'\0');

    const std::wstring work_dir_w = work_dir.wstring();
    const wchar_t* work_dir_ptr =
        work_dir_w.empty() ? nullptr : work_dir_w.c_str();

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!::CreateProcessW(exe_w.c_str(), cmd_buf.data(), nullptr, nullptr,
                          FALSE, CREATE_NO_WINDOW, nullptr, work_dir_ptr, &si,
                          &pi)) {
        return false;
    }
    ScopedHandle proc_guard(pi.hProcess);
    ScopedHandle thread_guard(pi.hThread);

    const DWORD wait_rc =
        ::WaitForSingleObject(proc_guard.get(), kDriverWaitTimeoutMs);
    if (wait_rc != WAIT_OBJECT_0) {
        ::TerminateProcess(proc_guard.get(), kTimeoutTerminateCode);
        return false;
    }
    return ::GetExitCodeProcess(proc_guard.get(), out_exit_code) != FALSE;
}

std::optional<fs::path> QueryVnGameDir() {
    const DWORD needed =
        ::GetEnvironmentVariableW(L"VN_GAME_DIR", nullptr, 0u);
    if (needed <= 1u) {
        return std::nullopt;
    }
    std::wstring val(static_cast<std::size_t>(needed), L'\0');
    const DWORD written =
        ::GetEnvironmentVariableW(L"VN_GAME_DIR", val.data(), needed);
    if (written == 0u || written >= needed) {
        return std::nullopt;
    }
    val.resize(static_cast<std::size_t>(written));
    return fs::path(val);
}

std::optional<std::size_t> MapRvaToFileOffset(const std::string& pe_bytes,
                                              uint32_t rva,
                                              std::size_t span) {
    if (pe_bytes.size() < sizeof(IMAGE_DOS_HEADER)) {
        return std::nullopt;
    }
    IMAGE_DOS_HEADER dos{};
    std::memcpy(&dos, pe_bytes.data(), sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0) {
        return std::nullopt;
    }

    const auto nt_offset = static_cast<std::size_t>(dos.e_lfanew);
    if (nt_offset + sizeof(IMAGE_NT_HEADERS32) > pe_bytes.size()) {
        return std::nullopt;
    }

    IMAGE_NT_HEADERS32 nt{};
    std::memcpy(&nt, pe_bytes.data() + nt_offset, sizeof(nt));
    if (nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        return std::nullopt;
    }

    const std::size_t sec_table_offset =
        nt_offset + offsetof(IMAGE_NT_HEADERS32, OptionalHeader) +
        nt.FileHeader.SizeOfOptionalHeader;
    const std::size_t sec_count = nt.FileHeader.NumberOfSections;
    if (sec_table_offset + sec_count * sizeof(IMAGE_SECTION_HEADER) >
        pe_bytes.size()) {
        return std::nullopt;
    }

    for (std::size_t i = 0u; i < sec_count; ++i) {
        IMAGE_SECTION_HEADER sec{};
        std::memcpy(&sec,
                    pe_bytes.data() + sec_table_offset +
                        i * sizeof(IMAGE_SECTION_HEADER),
                    sizeof(sec));
        const uint64_t sec_va_end =
            static_cast<uint64_t>(sec.VirtualAddress) + sec.SizeOfRawData;
        if (rva >= sec.VirtualAddress && (rva + span) <= sec_va_end) {
            const std::size_t file_off =
                static_cast<std::size_t>(sec.PointerToRawData) +
                static_cast<std::size_t>(rva - sec.VirtualAddress);
            if (file_off + span <= pe_bytes.size()) {
                return file_off;
            }
            return std::nullopt;
        }
    }
    return std::nullopt;
}

}  // namespace

TEST(ProxyDllSystem, DriverLoadsDllAndDllMainWritesBanner) {
    ASSERT_STRNE(PROXY_DLL_PATH, "");
    ASSERT_STRNE(DRIVER_EXE_PATH, "");
    ASSERT_STRNE(LOG_FILE_PATH, "");
    ASSERT_STRNE(EXPECTED_BANNER, "");

    const ScopedTempDir temp_dir("adieu_proxy_dll_st_");
    const fs::path driver_dir = fs::u8path(DRIVER_EXE_PATH).parent_path();

    // Ensure debug dump artifacts are absent around both CWD and driver_dir.
    const ScopedFileRemover driver_cap_guard(driver_dir /
                                             "adieu_jp_strings.txt");
    const ScopedFileRemover driver_key_guard(driver_dir /
                                             "siglus_key_dump.txt");

    // Stage a synthetic 3-row translation_table.tsv in the isolated CWD.
    {
        std::ofstream tsv(temp_dir.path() / "translation_table.tsv",
                          std::ios::binary);
        tsv << "人見\tHitomi\n"
            << "睦　月\tMutsuki\n"
            << "　夢だ。\tIt was a dream.\n";
    }

    const fs::path configured_log = fs::u8path(LOG_FILE_PATH);
    const fs::path active_log_path = configured_log.is_absolute()
                                         ? configured_log
                                         : (temp_dir.path() / configured_log);
    const ScopedFileRemover active_log_guard(active_log_path);

    DWORD exit_code = kSentinelExitCode;
    ASSERT_TRUE(RunDriverExe(DRIVER_EXE_PATH, PROXY_DLL_PATH, temp_dir.path(),
                             &exit_code))
        << "driver EXE failed to spawn or timed out (gle=" << ::GetLastError()
        << ")";
    EXPECT_EQ(exit_code, kCleanExitCode);

    const std::string log = ReadAllBytes(active_log_path);
    ASSERT_FALSE(log.empty())
        << "log file empty or missing: " << active_log_path.u8string();

    EXPECT_NE(log.find(EXPECTED_BANNER), std::string::npos) << log;
    EXPECT_NE(log.find("Loaded 3 entries"), std::string::npos) << log;
    EXPECT_NE(log.find("Host process does not match SiglusEngine.exe prologues"),
              std::string::npos)
        << log;
    EXPECT_NE(log.find("[Chrome] Patched "), std::string::npos) << log;

    // Debug capture and key-dump files must NOT be created by default.
    EXPECT_FALSE(fs::exists(temp_dir.path() / "adieu_jp_strings.txt"));
    EXPECT_FALSE(fs::exists(temp_dir.path() / "siglus_key_dump.txt"));
    EXPECT_FALSE(fs::exists(driver_dir / "adieu_jp_strings.txt"));
    EXPECT_FALSE(fs::exists(driver_dir / "siglus_key_dump.txt"));
}

TEST(ProxyDllSystem,
     RetailSiglusEngineMatchesVerifiedHookProloguesAndCallingConventions) {
    const auto game_dir = QueryVnGameDir();
    if (!game_dir.has_value()) {
        GTEST_SKIP()
            << "VN_GAME_DIR not set; skipping retail SiglusEngine.exe prologue check";
    }

    const fs::path exe_path = *game_dir / "SiglusEngine.exe";
    ASSERT_TRUE(fs::exists(exe_path))
        << "SiglusEngine.exe not found under VN_GAME_DIR: "
        << exe_path.u8string();

    const std::string pe_bytes = ReadAllBytes(exe_path);
    ASSERT_FALSE(pe_bytes.empty());

    // 1. RVA 0x10C3A0: std::wstring range routine prologue + `ret 0x0C` at +0xCC
    const auto assign_off = MapRvaToFileOffset(
        pe_bytes, kWstringAssignRangeRva,
        kWstringAssignRetOffset + sizeof(kExpectedRet0C));
    ASSERT_TRUE(assign_off.has_value());
    EXPECT_EQ(std::memcmp(pe_bytes.data() + *assign_off, kExpectedSehPrologue,
                          sizeof(kExpectedSehPrologue)),
              0);
    EXPECT_EQ(std::memcmp(
                  pe_bytes.data() + *assign_off + kWstringAssignRetOffset,
                  kExpectedRet0C, sizeof(kExpectedRet0C)),
              0);

    // 2. RVA 0x14C8E0: Scene::get_str prologue, call to 0x10C3A0 at +0xBA, and `ret 0x0C`
    const auto get_str_off = MapRvaToFileOffset(
        pe_bytes, kSceneGetStrRva, kSceneGetStrRetOffset + sizeof(kExpectedRet0C));
    ASSERT_TRUE(get_str_off.has_value());
    EXPECT_EQ(std::memcmp(pe_bytes.data() + *get_str_off, kExpectedSehPrologue,
                          sizeof(kExpectedSehPrologue)),
              0);
    EXPECT_EQ(std::memcmp(pe_bytes.data() + *get_str_off + kSceneGetStrRetOffset,
                          kExpectedRet0C, sizeof(kExpectedRet0C)),
              0);

    const std::size_t call_site_off =
        *get_str_off + kSceneGetStrCallAssignOffset;
    ASSERT_EQ(static_cast<uint8_t>(pe_bytes[call_site_off]), kCallRel32Opcode);
    int32_t call_rel32 = 0;
    std::memcpy(&call_rel32, pe_bytes.data() + call_site_off + 1u,
                sizeof(call_rel32));
    const uint32_t call_target_rva = static_cast<uint32_t>(
        static_cast<int64_t>(kSceneGetStrRva + kSceneGetStrCallAssignOffset +
                             5u) +
        call_rel32);
    EXPECT_EQ(call_target_rva, kWstringAssignRangeRva);

    // 3. RVA 0x14C9C0: Scene::get_cur_str prologue and unconditional SS_V2 0x7087 XOR multiplier
    const auto get_cur_str_off = MapRvaToFileOffset(
        pe_bytes, kSceneGetCurStrRva,
        kSceneGetCurStrSsV2XorOffset + sizeof(kExpectedSsV2Imul));
    ASSERT_TRUE(get_cur_str_off.has_value());
    EXPECT_EQ(std::memcmp(pe_bytes.data() + *get_cur_str_off,
                          kExpectedSehPrologue, sizeof(kExpectedSehPrologue)),
              0);
    EXPECT_EQ(std::memcmp(pe_bytes.data() + *get_cur_str_off +
                              kSceneGetCurStrSsV2XorOffset,
                          kExpectedSsV2Imul, sizeof(kExpectedSsV2Imul)),
              0);
}
