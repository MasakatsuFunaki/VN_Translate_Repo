// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Tier-2 game-specific live Gemini (`gemini-4-carbon`) SYSTEM test for
// EXTRAVAGANZA_06_YUMEMI_CE driving built `02_build_tsv.exe` inside a hermetic
// temporary directory on authentic BLACKCyc `.spt` script lines, `glossary.cpp`
// speakers (`夢美` -> `Yumemi`, `夢美１` -> `Yumemi 1`), `Speaker\r\n` nameplate
// formatting, CP932 `translation_table.tsv` generation, and 2nd-pass 0-API-call
// resume idempotency.
#include "build_tsv/build_tsv.h"
#include "cache/cache_core.h"
#include "cache/gemini_client.h"
#include "common/util.h"
#include "vn/llm/llm_types.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace bj = boost::json;
namespace fs = std::filesystem;

constexpr std::size_t kCachedPrologueDialogueCount = 10;
constexpr std::size_t kUncachedYumemiLineCount = 5;
constexpr std::size_t kBuildTsvCandidateCount = 3;
constexpr std::size_t kSampleYumemiIndex = 1;
constexpr std::size_t kSampleYumemi1Index = 3;
constexpr std::size_t kCrlfLen = 2;
constexpr int kMaxParentWalkLevels = 6;
constexpr int kLiveBatchSize = exm::translate::kDefaultBatchSize;
// Exceeds `vn::llm::kDefaultReadTimeoutSeconds` (600s) so transport read
// timeouts surface cleanly rather than getting killed prematurely.
constexpr DWORD kProcessTimeoutMs = 660000;
constexpr std::size_t kModulePathInitialCap = 512;
constexpr std::size_t kModulePathMaxCap = 32768;
constexpr std::size_t kGrowthFactor = 2;
constexpr int kJsonIndent = 2;

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view prefix) {
        static std::atomic<std::uint64_t> counter{0};
        const std::uint64_t id = counter.fetch_add(1u, std::memory_order_relaxed);
        path_ = fs::temp_directory_path() /
                fs::u8path(std::string(prefix) + "_" +
                           std::to_string(GetCurrentProcessId()) + "_" +
                           std::to_string(id));
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE h = INVALID_HANDLE_VALUE) noexcept : handle_(h) {}
    ~ScopedHandle() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    HANDLE get() const noexcept { return handle_; }
    bool valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_;
};

std::wstring utf8_to_wide(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int wide_len = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
        static_cast<int>(utf8.size()), nullptr, 0);
    if (wide_len <= 0) {
        throw std::runtime_error("MultiByteToWideChar failed");
    }
    std::wstring wide(static_cast<std::size_t>(wide_len), L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
        static_cast<int>(utf8.size()), wide.data(), wide_len);
    if (written != wide_len) {
        throw std::runtime_error("MultiByteToWideChar length mismatch");
    }
    return wide;
}

fs::path current_test_exe_dir() {
    std::wstring buf(kModulePathInitialCap, L'\0');
    for (;;) {
        const DWORD len = GetModuleFileNameW(
            nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0) {
            return {};
        }
        if (len < buf.size()) {
            buf.resize(static_cast<std::size_t>(len));
            return fs::path(buf).parent_path();
        }
        if (buf.size() >= kModulePathMaxCap) {
            return {};
        }
        buf.resize(buf.size() * kGrowthFactor, L'\0');
    }
}

fs::path resolve_built_build_tsv_exe() {
    const fs::path exe_dir = current_test_exe_dir();
    if (exe_dir.empty()) {
        return {};
    }
    const std::array<fs::path, kBuildTsvCandidateCount> candidates = {
        exe_dir / ".." / ".." / "install" / "bin" / "02_build_tsv.exe",
        exe_dir / ".." / ".." / "Release" / "02_build_tsv.exe",
        exe_dir / "02_build_tsv.exe",
    };
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (fs::exists(candidate, ec) && !ec) {
            return fs::weakly_canonical(candidate, ec);
        }
    }
    return {};
}

fs::path find_checkout_project_dir() {
    fs::path cur = current_test_exe_dir();
    for (int i = 0; i < kMaxParentWalkLevels && !cur.empty(); ++i) {
        std::error_code ec;
        if (fs::exists(cur / "build.py", ec) &&
            fs::is_directory(cur / "pipeline_cpp", ec)) {
            return cur;
        }
        if (!cur.has_parent_path() || cur.parent_path() == cur) {
            break;
        }
        cur = cur.parent_path();
    }
    return {};
}

std::wstring quote_win32_arg(std::wstring_view arg) {
    std::wstring out;
    out.push_back(L'"');
    for (std::size_t i = 0; i < arg.size(); ++i) {
        std::size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++backslashes;
            ++i;
        }
        if (i == arg.size()) {
            out.append(backslashes * kGrowthFactor, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            out.append(backslashes * kGrowthFactor + 1u, L'\\');
            out.push_back(L'"');
            continue;
        }
        out.append(backslashes, L'\\');
        out.push_back(arg[i]);
    }
    out.push_back(L'"');
    return out;
}

DWORD run_build_tsv_logged(const fs::path& exe_path,
                           const std::vector<std::string>& args,
                           const fs::path& log_path) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    const std::wstring log_w = log_path.wstring();
    const ScopedHandle out_file(CreateFileW(
        log_w.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!out_file.valid()) {
        throw std::runtime_error("CreateFileW failed for child stdout log");
    }

    std::wstring cmd = quote_win32_arg(exe_path.wstring());
    for (const auto& a : args) {
        cmd.push_back(L' ');
        cmd += quote_win32_arg(utf8_to_wide(a));
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = out_file.get();
    si.hStdError = out_file.get();

    PROCESS_INFORMATION pi{};
    const std::wstring app_w = exe_path.wstring();
    if (!CreateProcessW(app_w.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        throw std::runtime_error("CreateProcessW failed launching 02_build_tsv.exe");
    }
    const ScopedHandle proc(pi.hProcess);
    const ScopedHandle thread(pi.hThread);

    const DWORD wait_rc = WaitForSingleObject(proc.get(), kProcessTimeoutMs);
    if (wait_rc != WAIT_OBJECT_0) {
        TerminateProcess(proc.get(), 1u);
        throw std::runtime_error(
            "02_build_tsv.exe timed out during live Gemini SYSTEM test");
    }

    DWORD exit_code = 1u;
    if (!GetExitCodeProcess(proc.get(), &exit_code)) {
        throw std::runtime_error("GetExitCodeProcess failed");
    }
    return exit_code;
}

std::string read_log_utf8(const fs::path& path) {
    const exm::Bytes bytes = exm::read_file(path.u8string());
    return std::string(bytes.begin(), bytes.end());
}

struct YumemiScriptCase {
    const char* type;
    const char* jp;
    const char* expected_speaker_plate;
    bool expects_trailing_crlf;
};

TEST(LiveArgonSystemTest,
     TranslatesAuthenticYumemiSptBatchPreservesPlatesCp932AndResumesWithZeroApiCalls) {
    const ScopedTempDir sandbox("yumemi_st_live_argon");
    const fs::path temp_project = sandbox.path() / "project";
    const fs::path temp_out_dir = temp_project / "script_output";
    const fs::path temp_game = sandbox.path() / "game";
    fs::create_directories(temp_out_dir);
    fs::create_directories(temp_game);

    const fs::path checkout_dir = find_checkout_project_dir();
    if (!checkout_dir.empty()) {
        exm::gemini::load_api_key(checkout_dir.u8string());
    } else {
        exm::gemini::load_api_key(temp_project.u8string());
    }

    const char* api_key = std::getenv("GEMINI_API_KEY");
    const bool has_key = (api_key != nullptr && *api_key != '\0');
    if (!has_key) {
        const char* gh_actions = std::getenv("GITHUB_ACTIONS");
        if (gh_actions != nullptr && std::string_view(gh_actions) == "true") {
            GTEST_SKIP()
                << "GEMINI_API_KEY is absent on GitHub Actions (GITHUB_ACTIONS=true).";
        }
        FAIL() << "GEMINI_API_KEY is missing locally; live gemini-4-carbon SYSTEM test "
                  "must not skip outside GITHUB_ACTIONS=true.";
    }

    const fs::path build_tsv_exe = resolve_built_build_tsv_exe();
    ASSERT_FALSE(build_tsv_exe.empty())
        << "Built 02_build_tsv.exe not found relative to test executable.";
    ASSERT_TRUE(fs::exists(build_tsv_exe)) << build_tsv_exe.u8string();

    // Authentic `m_tokuten.spt` BLACKCyc System-NNN strings:
    // - Pre-cached prologue dialogue satisfies the 5-check speaker gate with 0 extra spend.
    // - Uncached batch covers `%#黄` color prefix, multi-line `夢美\r\n` dialogue with
    //   trailing `\r\n`, `夢美１\r\n` (`Yumemi 1`) glossary speaker resolution, and
    //   interactive command narrative, followed by a cached lookahead choice line.
    constexpr const char* kCachedSeedJp =
        "夢美\r\n「くうっ、や、やめてぇっ！」\r\n";
    constexpr const char* kCachedSeedEn =
        "Yumemi\r\n\"Nngh, s-stop it!\"\r\n";
    constexpr const char* kLookaheadJp = "苦痛を中心に責める";
    constexpr const char* kLookaheadEn = "Focus on inflicting pain";

    constexpr std::array<YumemiScriptCase, kUncachedYumemiLineCount> kSnippet = {{
        {"narrative", "%#黄エターナルモードを使用しますか？", nullptr, false},
        {"dialogue",
         "夢美\r\n「い、いやっ、放してっ！！\r\n　一体、何を……何をするつもりなのっ！？」\r\n",
         "Yumemi\r\n",
         true},
        {"dialogue",
         "夢美\r\n「はぁっ、はぁっ、はぁっ、はぁっ、はぁっ…………。\r\n　お、お願い……これ以上は……もう…………」\r\n",
         "Yumemi\r\n",
         true},
        {"dialogue",
         "夢美１\r\n「お、お願いです……これ以上は、許してください……！」\r\n",
         "Yumemi 1\r\n",
         true},
        {"narrative", "快楽を中心に責める", nullptr, false},
    }};

    bj::array strings;
    for (std::size_t i = 0; i < kCachedPrologueDialogueCount; ++i) {
        strings.push_back(bj::object{
            {"type", "dialogue"},
            {"text", kCachedSeedJp},
        });
    }
    for (const auto& item : kSnippet) {
        strings.push_back(bj::object{
            {"type", item.type},
            {"text", item.jp},
        });
    }
    strings.push_back(bj::object{
        {"type", "narrative"},
        {"text", kLookaheadJp},
    });

    bj::object file_obj;
    file_obj["strings"] = std::move(strings);
    bj::object extracted_root;
    extracted_root["m_tokuten.spt"] = std::move(file_obj);
    exm::write_file_text(
        (temp_out_dir / "extracted_text.json").u8string(),
        exm::json_pretty(bj::value(std::move(extracted_root)), kJsonIndent));

    bj::object initial_cache;
    initial_cache[kCachedSeedJp] = kCachedSeedEn;
    initial_cache[kLookaheadJp] = kLookaheadEn;
    exm::write_file_text(
        (temp_out_dir / "translation_cache_gemini.json").u8string(),
        exm::json_pretty(bj::value(std::move(initial_cache)), kJsonIndent));

    const std::vector<std::string> cli_args = {
        "--dir",
        temp_project.u8string(),
        "--game-dir",
        temp_game.u8string(),
        "--batch",
        std::to_string(kLiveBatchSize),
    };

    // Pass 1: Live `gemini-4-carbon` translation + CP932 table generation.
    const fs::path pass1_log_path = sandbox.path() / "pass1_stdout.log";
    const DWORD pass1_rc =
        run_build_tsv_logged(build_tsv_exe, cli_args, pass1_log_path);
    const std::string pass1_log = read_log_utf8(pass1_log_path);
    ASSERT_EQ(pass1_rc, 0u) << "Pass 1 02_build_tsv.exe failed:\n" << pass1_log;
    EXPECT_EQ(pass1_log.find("[ERROR]"), std::string::npos) << pass1_log;
    EXPECT_NE(pass1_log.find("--- API batch #1 (5 new lines) ---"),
              std::string::npos)
        << pass1_log;

    const fs::path last_tr_path =
        temp_out_dir / "last_gemini_translate" / "last_translation.json";
    ASSERT_TRUE(fs::exists(last_tr_path)) << pass1_log;
    const bj::value last_tr_val = exm::json_parse_file(last_tr_path.u8string());
    ASSERT_TRUE(last_tr_val.is_object());
    const bj::object& last_tr = last_tr_val.as_object();

    ASSERT_NE(last_tr.if_contains("model"), nullptr);
    EXPECT_EQ(std::string_view(last_tr.at("model").as_string()),
              vn::llm::kDefaultModel);
    ASSERT_NE(last_tr.if_contains("thinking_level"), nullptr);
    EXPECT_EQ(std::string_view(last_tr.at("thinking_level").as_string()),
              vn::llm::kDefaultThinkingLevel);

    ASSERT_NE(last_tr.if_contains("thought_tokens"), nullptr);
    ASSERT_TRUE(last_tr.at("thought_tokens").is_int64());
    const std::int64_t thought_tokens = last_tr.at("thought_tokens").as_int64();
    const std::int64_t input_tokens = last_tr.at("input_tokens").as_int64();
    const std::int64_t output_tokens = last_tr.at("output_tokens").as_int64();
    EXPECT_GT(thought_tokens, 0)
        << "Expected active MEDIUM reasoning thoughtsTokenCount > 0";
    EXPECT_NE(pass1_log.find("thoughts=" + std::to_string(thought_tokens)),
              std::string::npos)
        << pass1_log;

    const fs::path cache_path = temp_out_dir / "translation_cache_gemini.json";
    ASSERT_TRUE(fs::exists(cache_path));
    const bj::value cache_val = exm::json_parse_file(cache_path.u8string());
    ASSERT_TRUE(cache_val.is_object());
    const bj::object& cache_obj = cache_val.as_object();

    for (const auto& item : kSnippet) {
        SCOPED_TRACE(item.jp);
        const auto* entry = cache_obj.if_contains(item.jp);
        ASSERT_NE(entry, nullptr) << "Missing cached translation";
        ASSERT_TRUE(entry->is_string());
        const std::string en(entry->as_string());
        EXPECT_FALSE(en.empty());
        EXPECT_NE(en, item.jp);

        // Verify strict CP932 round-trip cleanliness on every translated string.
        const std::string cp932_bytes = exm::utf8_to_cp932(en);
        const exm::Bytes raw_en(cp932_bytes.begin(), cp932_bytes.end());
        const auto roundtrip_utf8 = exm::cp932_to_utf8_strict(raw_en);
        ASSERT_TRUE(roundtrip_utf8.has_value())
            << "Translated output is not strict CP932: " << en;
        EXPECT_EQ(*roundtrip_utf8, en);

        if (item.expected_speaker_plate != nullptr) {
            EXPECT_EQ(en.rfind(item.expected_speaker_plate, 0), 0u)
                << "Expected Speaker\\r\\n plate '" << item.expected_speaker_plate
                << "' on translated dialogue, got: " << en;
        }
        if (item.expects_trailing_crlf) {
            ASSERT_GE(en.size(), kCrlfLen);
            EXPECT_EQ(en.compare(en.size() - kCrlfLen, kCrlfLen, "\r\n"), 0)
                << "Expected trailing \\r\\n preserved on: " << en;
        }
    }

    const fs::path tsv_path = temp_game / "translation_table.tsv";
    ASSERT_TRUE(fs::exists(tsv_path));
    const exm::Bytes tsv_bytes_pass1 = exm::read_file(tsv_path.u8string());
    ASSERT_FALSE(tsv_bytes_pass1.empty());
    const auto tsv_utf8 = exm::cp932_to_utf8_strict(tsv_bytes_pass1);
    ASSERT_TRUE(tsv_utf8.has_value()) << "translation_table.tsv is not valid CP932";

    for (const auto& item : kSnippet) {
        const std::string escaped_key = exm::build_tsv::escape_for_tsv(item.jp) + "\t";
        const std::size_t row_pos = tsv_utf8->find(escaped_key);
        ASSERT_NE(row_pos, std::string::npos)
            << "Missing TSV row for key: " << escaped_key;
        const std::size_t row_end = tsv_utf8->find('\n', row_pos);
        const std::string row_en = tsv_utf8->substr(
            row_pos + escaped_key.size(),
            row_end == std::string::npos
                ? std::string::npos
                : row_end - (row_pos + escaped_key.size()));
        EXPECT_FALSE(row_en.empty());
        EXPECT_EQ(std::count(row_en.begin(), row_en.end(), '\t'), 0) << row_en;
    }

    const std::string sample_yumemi_en(
        cache_obj.at(kSnippet[kSampleYumemiIndex].jp).as_string());
    const std::string sample_yumemi1_en(
        cache_obj.at(kSnippet[kSampleYumemi1Index].jp).as_string());
    std::cout << "[LiveArgon] in=" << input_tokens << " out=" << output_tokens
              << " thought_tokens=" << thought_tokens << "\n"
              << "[LiveArgon] yumemi_en=" << sample_yumemi_en
              << "[LiveArgon] yumemi1_en=" << sample_yumemi1_en << std::endl;

    // Pass 2: Immediate 2nd-pass resume must perform 0 API calls and preserve
    // cache, telemetry, and CP932 TSV artifacts byte-for-byte.
    const exm::Bytes cache_bytes_pass1 = exm::read_file(cache_path.u8string());
    const exm::Bytes last_tr_bytes_pass1 = exm::read_file(last_tr_path.u8string());

    const fs::path pass2_log_path = sandbox.path() / "pass2_stdout.log";
    const DWORD pass2_rc =
        run_build_tsv_logged(build_tsv_exe, cli_args, pass2_log_path);
    const std::string pass2_log = read_log_utf8(pass2_log_path);
    ASSERT_EQ(pass2_rc, 0u) << "Pass 2 resume failed:\n" << pass2_log;
    EXPECT_EQ(pass2_log.find("[ERROR]"), std::string::npos) << pass2_log;
    EXPECT_NE(pass2_log.find("Need translating: 0"), std::string::npos) << pass2_log;
    EXPECT_NE(pass2_log.find("Everything is already cached!"), std::string::npos)
        << pass2_log;
    EXPECT_EQ(pass2_log.find("--- API batch #"), std::string::npos)
        << "Expected 0 API batches on 2nd-pass resume:\n"
        << pass2_log;
    EXPECT_EQ(pass2_log.find("Tokens: in="), std::string::npos)
        << "Expected 0 token-spending API calls on 2nd-pass resume:\n"
        << pass2_log;

    EXPECT_EQ(exm::read_file(cache_path.u8string()), cache_bytes_pass1);
    EXPECT_EQ(exm::read_file(last_tr_path.u8string()), last_tr_bytes_pass1);
    EXPECT_EQ(exm::read_file(tsv_path.u8string()), tsv_bytes_pass1);
}

}  // namespace
