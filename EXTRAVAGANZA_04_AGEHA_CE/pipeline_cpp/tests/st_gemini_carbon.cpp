// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Game-specific Tier-2 live Gemini Carbon system test for AGEHA ~EXTRAVAGANZA~ CE:
// runs built 02_build_tsv.exe inside a hermetic temporary directory against
// authentic BLACKCyc .spt script lines, speaker glossary plates (Speaker\r\n),
// internal \r\n line breaks, CP932 runtime table encoding, and 0-API-call
// second-pass resume idempotency.
#include "apps/paths.h"
#include "build_tsv/build_tsv.h"
#include "cache/cache_core.h"
#include "cache/gemini_client.h"
#include "cache/glossary.h"
#include "common/util.h"
#include "vn/llm/llm_types.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace bj = boost::json;

constexpr std::size_t kPipeReadChunkBytes = 4096;
constexpr DWORD kMillisecondsPerSecond = 1000U;
constexpr DWORD kChildTimeoutMs =
    static_cast<DWORD>(vn::llm::kDefaultReadTimeoutSeconds) * kMillisecondsPerSecond;
constexpr int kLiveMaxBatches = 1;
constexpr int kJsonIndentSpaces = 2;
constexpr std::size_t kCrlfLen = 2;
constexpr std::size_t kMaxExeSearchCandidates = 3;
constexpr std::string_view kEscapedCrlfLiteral = "\\r\\n";
constexpr std::size_t kEscapedCrlfLen = kEscapedCrlfLiteral.size();

constexpr std::size_t kUncachedAgehaLines = 6;
constexpr std::size_t kPreCachedLookaheadLines = 2;
constexpr std::size_t kTotalMainFixtureLines =
    kUncachedAgehaLines + kPreCachedLookaheadLines;

struct AgehaFixtureLine {
    const char* type;
    const char* jp_text;
    const char* expected_en_plate_prefix;
    const char* pre_cached_en;
};

constexpr std::array<AgehaFixtureLine, kTotalMainFixtureLines> kAgehaMainScene = {{
    {"narrative",
     "そもそもの間違いを考えれば、蟲という存在と\r\n関わってしまった事だったのかもしれない。\r\n",
     "",
     nullptr},
    {"narrative",
     "だが、その西の助手についていた北畠史郎の存在が、\r\nすべてを悪夢へと運び入れる。\r\n",
     "",
     nullptr},
    {"dialogue",
     "？？？\r\n「あなたと同じ。捕虜のひとりよ」\r\n",
     "???\r\n",
     nullptr},
    {"dialogue",
     "アゲハ\r\n「あ、もしかして、アンタもアイツに捕まったの？」\r\n",
     "Ageha\r\n",
     nullptr},
    {"dialogue",
     "史郎\r\n「やあ、目が覚めたかい？」\r\n",
     "Shirou\r\n",
     nullptr},
    {"dialogue",
     "アゲハ\r\n「な、なん、で……ここ、どこ……？」\r\n",
     "Ageha\r\n",
     nullptr},
    {"dialogue",
     "アゲハ\r\n「……ッ！」\r\n",
     "Ageha\r\n",
     "Ageha\r\n\"...!\"\r\n"},
    {"dialogue",
     "？？？\r\n「貴様ッ！！」\r\n",
     "???\r\n",
     "???\r\n\"You bastard!!\"\r\n"},
}};

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE h = nullptr) noexcept : handle_(h) {}
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ~ScopedHandle() { reset(); }

    HANDLE get() const noexcept { return handle_; }

    void reset(HANDLE next = nullptr) noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        handle_ = next;
    }

private:
    HANDLE handle_;
};

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view prefix)
        : path_(fs::temp_directory_path() /
                fs::u8path(std::string(prefix) + "_" +
                           std::to_string(GetCurrentProcessId()))) {
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
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

struct ProcessResult {
    int exit_code = -1;
    double elapsed_seconds = 0.0;
    std::string output;
};

std::wstring utf8_to_wide_checked(std::string_view s) {
    if (s.empty()) {
        return {};
    }
    const int len = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
        static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) {
        throw std::runtime_error("UTF-8 to UTF-16 conversion failed");
    }
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                        static_cast<int>(s.size()), out.data(), len);
    return out;
}

ProcessResult run_process_capturing_output(const fs::path& exe_path,
                                           const std::vector<std::string>& args,
                                           const fs::path& working_dir) {
    const auto t0 = std::chrono::steady_clock::now();

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE raw_read = nullptr;
    HANDLE raw_write = nullptr;
    if (!CreatePipe(&raw_read, &raw_write, &sa, 0)) {
        throw std::runtime_error("CreatePipe failed");
    }
    ScopedHandle read_pipe(raw_read);
    ScopedHandle write_pipe(raw_write);

    if (!SetHandleInformation(read_pipe.get(), HANDLE_FLAG_INHERIT, 0)) {
        throw std::runtime_error("SetHandleInformation failed");
    }

    std::wstring cmd_line =
        exm::apps::build_windows_command_line(exe_path.u8string(), args);
    const std::wstring wide_exe = utf8_to_wide_checked(exe_path.u8string());
    const std::wstring wide_cwd = utf8_to_wide_checked(working_dir.u8string());

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = write_pipe.get();
    si.hStdError = write_pipe.get();

    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(wide_exe.c_str(), cmd_line.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, wide_cwd.c_str(), &si, &pi)) {
        throw std::runtime_error("CreateProcessW failed for " + exe_path.u8string());
    }
    ScopedHandle proc_guard(pi.hProcess);
    ScopedHandle thread_guard(pi.hThread);
    write_pipe.reset();

    std::string captured;
    std::array<char, kPipeReadChunkBytes> chunk{};
    for (;;) {
        DWORD bytes_read = 0;
        const BOOL ok = ReadFile(read_pipe.get(), chunk.data(),
                                 static_cast<DWORD>(chunk.size()), &bytes_read, nullptr);
        if (!ok || bytes_read == 0) {
            break;
        }
        captured.append(chunk.data(), bytes_read);
    }

    const DWORD wait_rc = WaitForSingleObject(proc_guard.get(), kChildTimeoutMs);
    if (wait_rc != WAIT_OBJECT_0) {
        TerminateProcess(proc_guard.get(), 1);
        throw std::runtime_error("02_build_tsv.exe timed out; partial output:\n" +
                                 captured);
    }

    DWORD exit_code = 0;
    if (!GetExitCodeProcess(proc_guard.get(), &exit_code)) {
        throw std::runtime_error("GetExitCodeProcess failed");
    }
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return ProcessResult{static_cast<int>(exit_code), elapsed, std::move(captured)};
}

fs::path resolve_build_tsv_exe() {
    const fs::path dir = fs::u8path(exm::apps::exe_dir());
    const std::array<fs::path, kMaxExeSearchCandidates> candidates = {
        dir / ".." / ".." / ".." / "install" / "bin" / "02_build_tsv.exe",
        dir / ".." / ".." / "Release" / "02_build_tsv.exe",
        dir / "02_build_tsv.exe",
    };
    std::error_code ec;
    for (const auto& candidate : candidates) {
        if (fs::is_regular_file(candidate, ec)) {
            return fs::weakly_canonical(candidate, ec);
        }
    }
    return {};
}

std::string to_ascii_lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

bool contains_any_substring(std::string_view haystack_lower,
                            std::initializer_list<std::string_view> needles) {
    return std::any_of(needles.begin(), needles.end(),
                       [&](std::string_view needle) {
                           return haystack_lower.find(needle) != std::string_view::npos;
                       });
}

std::optional<std::int64_t> parse_logged_thoughts(std::string_view log_text) {
    constexpr std::string_view kMarker = "thoughts=";
    const std::size_t pos = log_text.find(kMarker);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    const char* first = log_text.data() + pos + kMarker.size();
    const char* last = log_text.data() + log_text.size();
    std::int64_t val = 0;
    const auto [ptr, ec] = std::from_chars(first, last, val);
    if (ec != std::errc{} || ptr == first) {
        return std::nullopt;
    }
    return val;
}

void write_ageha_hermetic_fixture(const fs::path& root_dir) {
    const fs::path out_dir = root_dir / "script_output";
    fs::create_directories(out_dir);

    bj::array main_strings;
    exm::translate::Cache seeded_cache;
    for (std::size_t i = 0; i < kAgehaMainScene.size(); ++i) {
        const AgehaFixtureLine& row = kAgehaMainScene[i];
        main_strings.push_back(bj::object{
            {"index", static_cast<std::int64_t>(i)},
            {"type", row.type},
            {"text", row.jp_text},
        });
        if (row.pre_cached_en != nullptr) {
            seeded_cache.set(row.jp_text, row.pre_cached_en);
        }
    }

    bj::array sys_strings;
    std::size_t speaker_idx = 0;
    for (const auto& [jp_name, en_name] : exm::translate::name_translations_ordered()) {
        const std::string jp_line = jp_name + "\r\n「システム確認。」\r\n";
        const std::string en_line = en_name + "\r\n\"System check.\"\r\n";
        sys_strings.push_back(bj::object{
            {"index", static_cast<std::int64_t>(speaker_idx++)},
            {"type", "dialogue"},
            {"text", jp_line},
        });
        seeded_cache.set(jp_line, en_line);
    }

    bj::object extracted{
        {"main.spt", bj::object{{"strings", std::move(main_strings)}}},
        {"sys.spt", bj::object{{"strings", std::move(sys_strings)}}},
    };
    exm::write_file_text(
        (out_dir / "extracted_text.json").u8string(),
        exm::json_pretty(bj::value(std::move(extracted)), kJsonIndentSpaces));
    exm::translate::save_cache(
        seeded_cache, (out_dir / "translation_cache_gemini.json").u8string());
}

}  // namespace

TEST(GeminiCarbonSystem, LiveAgehaScriptE2ESpeakerPlatesCp932AndZeroCallResume) {
    const ScopedTempDir temp_dir("ageha_st_gemini_carbon");

    exm::gemini::load_api_key(temp_dir.path().u8string());
    const auto api_key = exm::apps::wide_env_utf8(L"GEMINI_API_KEY");
    if (!api_key.has_value()) {
        const auto gh_actions = exm::apps::wide_env_utf8(L"GITHUB_ACTIONS");
        if (gh_actions.has_value() && *gh_actions == "true") {
            GTEST_SKIP() << "Skipping live gemini-4-carbon SYSTEM test on GitHub Actions "
                            "because GEMINI_API_KEY is absent.";
        }
        FAIL() << "GEMINI_API_KEY is missing; live gemini-4-carbon SYSTEM test must fail "
                  "loudly on local runs.";
    }

    const fs::path build_tsv_exe = resolve_build_tsv_exe();
    ASSERT_FALSE(build_tsv_exe.empty())
        << "Built 02_build_tsv.exe not found relative to " << exm::apps::exe_dir();

    write_ageha_hermetic_fixture(temp_dir.path());
    const fs::path game_dir = temp_dir.path() / "game";
    fs::create_directories(game_dir);

    const std::vector<std::string> args = {
        "--dir", temp_dir.path().u8string(),
        "--game-dir", game_dir.u8string(),
        "--batch", std::to_string(exm::translate::kDefaultBatchSize),
        "--max-batches", std::to_string(kLiveMaxBatches),
    };

    const ProcessResult pass1 =
        run_process_capturing_output(build_tsv_exe, args, temp_dir.path());

    ASSERT_EQ(pass1.exit_code, exm::apps::kExitOk) << pass1.output;
    EXPECT_LT(pass1.elapsed_seconds,
              static_cast<double>(vn::llm::kDefaultReadTimeoutSeconds));
    EXPECT_EQ(pass1.output.find("[ERROR]"), std::string::npos) << pass1.output;
    EXPECT_EQ(pass1.output.find("Batch error:"), std::string::npos) << pass1.output;
    EXPECT_EQ(pass1.output.find("Skipped (CP932 encode error)"), std::string::npos)
        << pass1.output;
    EXPECT_NE(pass1.output.find("--- API batch #1 (6 new lines) ---"),
              std::string::npos)
        << pass1.output;

    const auto logged_thoughts = parse_logged_thoughts(pass1.output);
    ASSERT_TRUE(logged_thoughts.has_value())
        << "Expected thoughts=<N> token telemetry in 02_build_tsv output:\n"
        << pass1.output;
    EXPECT_GT(*logged_thoughts, 0) << pass1.output;

    const fs::path last_json_path = temp_dir.path() / "script_output" /
                                    "last_gemini_translate" / "last_translation.json";
    ASSERT_TRUE(fs::exists(last_json_path));
    const bj::value last_doc = exm::json_parse_file(last_json_path.u8string());
    ASSERT_TRUE(last_doc.is_object());
    const auto* thought_field = last_doc.get_object().if_contains("thought_tokens");
    ASSERT_NE(thought_field, nullptr);
    ASSERT_TRUE(thought_field->is_int64());
    const std::int64_t persisted_thoughts = thought_field->get_int64();
    EXPECT_GT(persisted_thoughts, 0);
    EXPECT_EQ(persisted_thoughts, *logged_thoughts);

    const fs::path cache_path =
        temp_dir.path() / "script_output" / "translation_cache_gemini.json";
    const exm::Bytes pass1_cache_bytes = exm::read_file(cache_path.u8string());
    const exm::translate::Cache updated_cache =
        exm::translate::load_cache(cache_path.u8string());

    const fs::path tsv_path = game_dir / "translation_table.tsv";
    ASSERT_TRUE(fs::exists(tsv_path));
    const exm::Bytes pass1_tsv_bytes = exm::read_file(tsv_path.u8string());
    const auto tsv_utf8 = exm::cp932_to_utf8_strict(pass1_tsv_bytes);
    ASSERT_TRUE(tsv_utf8.has_value())
        << "translation_table.tsv failed strict CP932 decoding";

    ASSERT_EQ(kAgehaMainScene.size(), kTotalMainFixtureLines);
    for (std::size_t i = 0; i < kUncachedAgehaLines; ++i) {
        SCOPED_TRACE(i);
        const AgehaFixtureLine& row = kAgehaMainScene[i];
        const std::string jp(row.jp_text);
        const std::string* en = updated_cache.get(jp);
        ASSERT_NE(en, nullptr) << "Missing translated cache entry for: " << jp;
        EXPECT_FALSE(en->empty());
        EXPECT_NE(*en, jp);
        EXPECT_FALSE(exm::is_jp(*en)) << "Residual Japanese characters in: " << *en;
        EXPECT_TRUE(exm::utf8_to_cp932_strict(*en).has_value())
            << "Translated cache entry is not 100% CP932-encodable: " << *en;
        ASSERT_GE(en->size(), kCrlfLen);
        EXPECT_EQ(en->substr(en->size() - kCrlfLen), "\r\n")
            << "Expected preserved trailing \\r\\n on: " << *en;

        const std::string_view expected_plate(row.expected_en_plate_prefix);
        if (!expected_plate.empty()) {
            EXPECT_EQ(en->rfind(std::string(expected_plate), 0), 0u)
                << "Expected English Speaker\\r\\n plate '" << expected_plate
                << "' on: " << *en;
        }

        const std::string tsv_key_prefix = exm::build_tsv::escape_for_tsv(jp) + "\t";
        const std::size_t tsv_pos = tsv_utf8->find(tsv_key_prefix);
        ASSERT_NE(tsv_pos, std::string::npos)
            << "Missing row in translation_table.tsv for index " << i;
        if (!expected_plate.empty()) {
            const std::string folded_speaker =
                std::string(expected_plate.substr(0, expected_plate.size() - kCrlfLen)) +
                ": ";
            const std::size_t line_end = tsv_utf8->find('\n', tsv_pos);
            const std::string tsv_val = tsv_utf8->substr(
                tsv_pos + tsv_key_prefix.size(),
                line_end == std::string::npos
                    ? std::string::npos
                    : line_end - (tsv_pos + tsv_key_prefix.size()));
            EXPECT_EQ(tsv_val.rfind(folded_speaker, 0), 0u)
                << "Expected folded speaker prefix '" << folded_speaker
                << "' in TSV value: " << tsv_val;
        }
    }

    EXPECT_TRUE(contains_any_substring(
        to_ascii_lower(*updated_cache.get(kAgehaMainScene[0].jp_text)),
        {"insect", "bug"}))
        << "Expected glossary term 'insect'/'bug' for 蟲 in line 0: "
        << *updated_cache.get(kAgehaMainScene[0].jp_text);
    const std::string line1_lower =
        to_ascii_lower(*updated_cache.get(kAgehaMainScene[1].jp_text));
    EXPECT_NE(line1_lower.find("nishi"), std::string::npos)
        << "Expected glossary name 'Nishi' in line 1: "
        << *updated_cache.get(kAgehaMainScene[1].jp_text);
    EXPECT_NE(line1_lower.find("shirou"), std::string::npos)
        << "Expected glossary name 'Shirou' in line 1: "
        << *updated_cache.get(kAgehaMainScene[1].jp_text);

    const ProcessResult pass2 =
        run_process_capturing_output(build_tsv_exe, args, temp_dir.path());

    ASSERT_EQ(pass2.exit_code, exm::apps::kExitOk) << pass2.output;
    EXPECT_EQ(pass2.output.find("[ERROR]"), std::string::npos) << pass2.output;
    EXPECT_NE(pass2.output.find("Need translating: 0"), std::string::npos)
        << pass2.output;
    EXPECT_NE(pass2.output.find("Everything is already cached!"), std::string::npos)
        << pass2.output;
    EXPECT_EQ(pass2.output.find("--- API batch #"), std::string::npos)
        << "Second-pass resume must make 0 API calls:\n" << pass2.output;
    EXPECT_EQ(pass2.output.find("Tokens: in="), std::string::npos)
        << "Second-pass resume must spend 0 tokens:\n" << pass2.output;

    EXPECT_EQ(exm::read_file(cache_path.u8string()), pass1_cache_bytes);
    EXPECT_EQ(exm::read_file(tsv_path.u8string()), pass1_tsv_bytes);

    exm::print_line("[ST telemetry] thought_tokens=" +
                    std::to_string(persisted_thoughts) +
                    " | pass1_sec=" + std::to_string(pass1.elapsed_seconds) +
                    " | pass2_api_calls=0");
    for (std::size_t i = 0; i < kUncachedAgehaLines; ++i) {
        const std::string* en = updated_cache.get(kAgehaMainScene[i].jp_text);
        if (en != nullptr) {
            std::string flat = *en;
            for (std::size_t p = 0; (p = flat.find("\r\n", p)) != std::string::npos;) {
                flat.replace(p, kCrlfLen, kEscapedCrlfLiteral.data(), kEscapedCrlfLen);
                p += kEscapedCrlfLen;
            }
            exm::print_line("[ST line " + std::to_string(i) + "] " + flat);
        }
    }
}
