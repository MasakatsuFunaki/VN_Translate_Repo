// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Game-specific Tier-2 live-API SYSTEM test exercising the built/staged
// `02_translate.exe` binary against live `gemini-4-carbon` (`thinkingLevel: "MEDIUM"`)
// over authentic `Sakura no Toki` (Artemis `.ast` engine) script lines, glossary
// speakers, Artemis 3-part ruby-split compounds, pinned terminology, leading
// U+3000 indent preservation, and 2nd-pass `0`-API-call resume idempotency.
//
// Explicit Environment Contract (clean-room-st Rule 3):
//   1. `GEMINI_API_KEY` must be configured (process env, `~/.gemini/.env`, or
//      `HKCU\Environment`).  Missing credentials FAIL LOUDLY locally (skipping
//      only when `GITHUB_ACTIONS == "true"`).
//   2. `02_translate.exe` must exist at `SNT_TRANSLATE_EXE` (or staged under
//      `<project>/build/install/bin/02_translate.exe`).
//   3. Live HTTPS connectivity to `generativelanguage.googleapis.com:443`.
#include "apps/paths.h"
#include "build_tsv/build_tsv.h"
#include "common/util.h"
#include "translate/gemini_client.h"
#include "translate/glossary.h"
#include "translate/speaker_gate.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace bj = boost::json;
namespace fs = std::filesystem;
namespace tr = snt::translate;

constexpr std::wstring_view kTempDirPrefix = L"桜ノ刻_st_live_";
constexpr DWORD kProcessTimeoutMs = 180000U;
constexpr DWORD kPipeReadChunkBytes = 4096U;
constexpr UINT kForcedTerminateExitCode = 1U;
constexpr int kExpectedExitCode = 0;
constexpr int kPrettyIndent = 2;
constexpr int kProductionBatchSize = vn::llm::kDefaultBatchSize;
constexpr std::int64_t kFirstEntryOffset = 1;
constexpr std::int64_t kDecimalRadix = 10;
constexpr std::ptrdiff_t kExpectedTsvTabs = 1;
constexpr std::size_t kLiveBatchSize = 8;
constexpr std::size_t kPreCachedContextAfterCount = 2;

constexpr std::size_t kIdxHekihiRubyBase = 1;
constexpr std::size_t kIdxTsuburagiRubyBase = 4;
constexpr std::size_t kIdxButterflyTitle = 6;
constexpr std::size_t kIdxYumihariIndented = 7;

struct ScriptSpec {
    const char* type;
    const char* speaker;  // nullptr -> narration (`speaker: null`)
    const char* text;
};

// Authentic Sakura no Toki (`script\01_01.ast` + cast scene) lines exercising:
//   - Artemis 3-part ruby-split glaze compound (`碧緋` -> `Hekihi`, speaker `直哉`)
//   - Artemis 3-part ruby-split person surname (`円木` -> `Tsuburagi`, speaker `静流`)
//   - Pinned artwork title (`『蝶を夢む』` -> `'Dreaming of a Butterfly'`, speaker `静流`)
//   - Core glossary cast (`稟`=`Rin`, `圭`=`Kei`, `藍`=`Ai`), pinned `弓張学園`
//     (`Yumihari Academy`), and leading U+3000 ideographic paragraph indent (`　`).
constexpr std::array<ScriptSpec, kLiveBatchSize> kSakuraNoTokiLines = {{
    {"dialogue", "直哉", "「だけど、一番重要なのはこの彩釉ですね。これは"},
    {"narrative", "直哉", "碧緋"},
    {"dialogue", "直哉", "と言われる独特なものです」"},
    {"dialogue", "静流", "「そうですか。"},
    {"narrative", "静流", "円木"},
    {"dialogue", "静流", "ちゃんが店長してた時からの常連さんですか？」"},
    {"dialogue", "静流", "「そうでもないよ。君だって大したもんじゃない。『蝶を夢む』なんか相当な名作よ」"},
    {"narrative", nullptr, "　弓張学園の美術室で、稟と圭、それに藍が静かに絵を見つめていた。"},
}};

// Pre-cached trailing lines from `script\01_01.ast` so Pass 1 includes
// `<context_after>` lookahead while Pass 2 sees `0` remaining uncached lines.
constexpr std::array<ScriptSpec, kPreCachedContextAfterCount> kPreCachedContextAfterLines = {{
    {"dialogue", "直哉", "「世界で彼だけがこの独特な彩釉、碧緋を出すことができる。"},
    {"dialogue", "静流", "「そうだね、たしかに吉沢緑寺先生はすごい人ではあるよ」"},
}};

constexpr const char* kPreCachedContextAfterEn0 =
    "\"He is the only person in the world capable of producing this distinctive glaze, Hekihi.";
constexpr const char* kPreCachedContextAfterEn1 =
    "\"Right, Yoshizawa Ryokuji-sensei really is an incredible person.\"";
constexpr const char* kPreCachedGreetingJp = "「おはよう」";
constexpr const char* kPreCachedGreetingEn = "\"Good morning.\"";

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE h = nullptr) noexcept : handle_(h) {}

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    ~ScopedHandle() { reset(); }

    void reset(HANDLE h = nullptr) noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        handle_ = h;
    }

    HANDLE get() const noexcept { return handle_; }

private:
    HANDLE handle_{nullptr};
};

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::wstring_view prefix) {
        static std::atomic<std::uint64_t> seq{0};
        const auto tick = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const DWORD pid = GetCurrentProcessId();
        const std::wstring leaf = std::wstring(prefix) + std::to_wstring(tick) +
                                  L"_" + std::to_wstring(pid) + L"_" +
                                  std::to_wstring(++seq);
        path_ = fs::temp_directory_path() / fs::path(leaf);
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    ~ScopedTempDir() noexcept {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

struct ProcessResult {
    int exit_code = -1;
    std::string output;
};

bool IsRunningInGithubActions() {
    const wchar_t* const ga = ::_wgetenv(L"GITHUB_ACTIONS");
    return ga != nullptr && std::wstring_view(ga) == L"true";
}

fs::path ResolveTranslateExecutable() {
    const fs::path built_exe = fs::u8path(SNT_TRANSLATE_EXE);
    const fs::path staged_exe =
        fs::u8path(SNT_PROJECT_DIR) / "build" / "install" / "bin" / "02_translate.exe";

    std::error_code ec;
    if (fs::exists(staged_exe, ec) && fs::exists(built_exe, ec)) {
        const auto staged_time = fs::last_write_time(staged_exe, ec);
        const auto built_time = fs::last_write_time(built_exe, ec);
        if (!ec && staged_time >= built_time) {
            return staged_exe;
        }
    }
    return built_exe;
}

ProcessResult RunProcessCapturingOutput(const fs::path& exe,
                                        const std::vector<std::string>& args) {
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
        throw std::runtime_error("SetHandleInformation failed on pipe read handle");
    }

    const std::wstring app_wide = exe.wstring();
    const std::string cmdline_utf8 = snt::apps::format_windows_command_line(
        snt::apps::wide_to_utf8(app_wide), args);
    std::wstring cmdline_wide = snt::apps::utf8_to_wide(cmdline_utf8);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = write_pipe.get();
    si.hStdError = write_pipe.get();

    PROCESS_INFORMATION pi{};
    const BOOL created = CreateProcessW(
        app_wide.c_str(), cmdline_wide.data(), nullptr, nullptr, TRUE, 0,
        nullptr, nullptr, &si, &pi);
    if (!created) {
        throw std::runtime_error(
            "CreateProcessW failed (Win32 error " + std::to_string(GetLastError()) + ")");
    }
    ScopedHandle proc_handle(pi.hProcess);
    ScopedHandle thread_handle(pi.hThread);

    write_pipe.reset();

    std::string captured;
    char chunk[kPipeReadChunkBytes];
    for (;;) {
        DWORD bytes_read = 0;
        const BOOL ok = ReadFile(
            read_pipe.get(), chunk, sizeof(chunk), &bytes_read, nullptr);
        if (!ok || bytes_read == 0) {
            break;
        }
        captured.append(chunk, static_cast<std::size_t>(bytes_read));
    }

    const DWORD wait_rc = WaitForSingleObject(proc_handle.get(), kProcessTimeoutMs);
    if (wait_rc != WAIT_OBJECT_0) {
        TerminateProcess(proc_handle.get(), kForcedTerminateExitCode);
        throw std::runtime_error(
            "02_translate.exe timed out or wait failed (rc=" +
            std::to_string(wait_rc) + ")\nOutput:\n" + captured);
    }

    DWORD exit_code = 0;
    if (!GetExitCodeProcess(proc_handle.get(), &exit_code)) {
        throw std::runtime_error("GetExitCodeProcess failed");
    }

    return ProcessResult{static_cast<int>(exit_code), std::move(captured)};
}

bj::object MakeEntry(const ScriptSpec& spec, std::int64_t offset) {
    bj::object e;
    e["type"] = spec.type;
    if (spec.speaker == nullptr) {
        e["speaker"] = nullptr;
    } else {
        e["speaker"] = spec.speaker;
    }
    e["text"] = spec.text;
    e["offset"] = offset;
    return e;
}

bj::object BuildSakuraNoTokiExtractionDocument() {
    bj::array live_strings;
    std::int64_t offset = kFirstEntryOffset;
    for (const auto& spec : kSakuraNoTokiLines) {
        live_strings.push_back(MakeEntry(spec, offset++));
    }
    for (const auto& spec : kPreCachedContextAfterLines) {
        live_strings.push_back(MakeEntry(spec, offset++));
    }

    bj::object live_script;
    live_script["jp_strings"] = static_cast<std::int64_t>(live_strings.size());
    live_script["strings"] = std::move(live_strings);

    bj::array roster_strings;
    for (const auto& [jp_speaker, en_speaker] : tr::name_translations_ordered()) {
        (void)en_speaker;
        bj::object plate;
        plate["type"] = "name";
        plate["text"] = jp_speaker;
        roster_strings.push_back(std::move(plate));

        bj::object dlg;
        dlg["type"] = "dialogue";
        dlg["speaker"] = jp_speaker;
        dlg["text"] = kPreCachedGreetingJp;
        roster_strings.push_back(std::move(dlg));
    }
    for (std::size_t i = 0; i < tr::DOSSIER_LINE_THRESHOLD; ++i) {
        bj::object dlg;
        dlg["type"] = "dialogue";
        dlg["speaker"] = "直哉";
        dlg["text"] = kPreCachedGreetingJp;
        roster_strings.push_back(std::move(dlg));
    }

    bj::object roster_script;
    roster_script["jp_strings"] = static_cast<std::int64_t>(roster_strings.size());
    roster_script["strings"] = std::move(roster_strings);

    bj::object root;
    root["script\\00_sakuranotoki_live.ast"] = std::move(live_script);
    root["script\\01_roster.ast"] = std::move(roster_script);
    return root;
}

std::optional<std::int64_t> ParseMetricToken(std::string_view text,
                                             std::string_view key_prefix) {
    const std::size_t pos = text.find(key_prefix);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    std::size_t cur = pos + key_prefix.size();
    if (cur >= text.size() || !std::isdigit(static_cast<unsigned char>(text[cur]))) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    while (cur < text.size() && std::isdigit(static_cast<unsigned char>(text[cur]))) {
        value = value * kDecimalRadix + (text[cur] - '0');
        ++cur;
    }
    return value;
}

std::string ToLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::map<std::string, std::string> ReadTsvRows(const fs::path& tsv_path) {
    std::ifstream in(tsv_path, std::ios::binary);
    EXPECT_TRUE(static_cast<bool>(in)) << tsv_path.u8string();
    std::map<std::string, std::string> rows;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        EXPECT_EQ(std::find(line.begin(), line.end(), '\0'), line.end()) << line;
        EXPECT_EQ(std::count(line.begin(), line.end(), '\t'), kExpectedTsvTabs) << line;
        const std::size_t tab = line.find('\t');
        if (tab != std::string::npos) {
            rows.emplace(line.substr(0, tab), line.substr(tab + 1));
        }
    }
    return rows;
}

}  // namespace

TEST(GeminiLiveSystemTest,
     SakuraNoTokiArtemisSliceTranslatesWithPositiveThoughtsAndZeroCallSecondPassResume) {
    if (snt::gemini::load_api_key().empty()) {
        if (IsRunningInGithubActions()) {
            GTEST_SKIP() << "GITHUB_ACTIONS=true and GEMINI_API_KEY unset; skipping live API ST";
        }
        FAIL() << "GEMINI_API_KEY is missing locally (checked GEMINI_API_KEY env var, "
                  "~/.gemini/.env, and HKCU\\Environment).";
    }

    const fs::path translate_exe = ResolveTranslateExecutable();
    ASSERT_TRUE(fs::exists(translate_exe))
        << "Built 02_translate.exe missing at: " << translate_exe.u8string();

    const ScopedTempDir sandbox(kTempDirPrefix);
    const fs::path out_dir = sandbox.path() / "script_output";
    const fs::path extracted_path = out_dir / "extracted_text.json";
    const fs::path cache_path = out_dir / "translation_cache_gemini.json";
    const fs::path translated_path = out_dir / "translated_text.json";
    const fs::path coverage_path = out_dir / "coverage_report.json";
    const fs::path raw_log_path = out_dir / "gemini_raw_answers.log";
    const fs::path tsv_path = sandbox.path() / "build" / "install" / "game" / "translations.tsv";

    snt::write_file(
        extracted_path.u8string(),
        snt::json_pretty(bj::value(BuildSakuraNoTokiExtractionDocument()), kPrettyIndent));

    bj::object seeded_cache;
    seeded_cache[kPreCachedGreetingJp] = kPreCachedGreetingEn;
    seeded_cache[kPreCachedContextAfterLines[0].text] = kPreCachedContextAfterEn0;
    seeded_cache[kPreCachedContextAfterLines[1].text] = kPreCachedContextAfterEn1;
    snt::write_file_text(
        cache_path.u8string(),
        snt::json_pretty(bj::value(std::move(seeded_cache)), kPrettyIndent));

    // Pass 1 translates the uncached Sakura no Toki lines against live Gemini.
    const auto pass1_start = std::chrono::steady_clock::now();
    const ProcessResult pass1 = RunProcessCapturingOutput(
        translate_exe,
        {"--dir", sandbox.path().u8string(),
         "--batch", std::to_string(kLiveBatchSize),
         "--max-batches", "1"});
    const double pass1_sec =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - pass1_start).count();

    ASSERT_EQ(pass1.exit_code, kExpectedExitCode)
        << "Pass 1 02_translate.exe failed:\n" << pass1.output;

    EXPECT_NE(pass1.output.find("Model: gemini-4-carbon"), std::string::npos) << pass1.output;
    EXPECT_NE(pass1.output.find("Reasoning: thinkingConfig.thinkingLevel: MEDIUM"),
              std::string::npos)
        << pass1.output;
    EXPECT_EQ(pass1.output.find("[ERROR]"), std::string::npos) << pass1.output;
    EXPECT_EQ(pass1.output.find("WARNING:"), std::string::npos) << pass1.output;
    EXPECT_LT(pass1_sec, static_cast<double>(snt::gemini::kDefaultTimeoutSec));

    ASSERT_TRUE(fs::exists(raw_log_path)) << "Missing gemini_raw_answers.log";
    const snt::Bytes raw_log_after_pass1 = snt::read_file(raw_log_path.u8string());
    const std::string raw_log(
        reinterpret_cast<const char*>(raw_log_after_pass1.data()),
        raw_log_after_pass1.size());

    const auto in_tokens = ParseMetricToken(raw_log, " in=");
    const auto out_tokens = ParseMetricToken(raw_log, " out=");
    const auto thought_tokens = ParseMetricToken(raw_log, " thought=");
    ASSERT_TRUE(in_tokens.has_value()) << raw_log;
    ASSERT_TRUE(out_tokens.has_value()) << raw_log;
    ASSERT_TRUE(thought_tokens.has_value()) << raw_log;

    GTEST_LOG_(INFO) << "Live gemini-4-carbon Sakura no Toki Pass 1: in=" << *in_tokens
                     << " out=" << *out_tokens
                     << " thought_tokens=" << *thought_tokens
                     << " elapsed_s=" << pass1_sec;

    ASSERT_GT(*thought_tokens, 0)
        << "gemini-4-carbon returned thought_tokens == 0:\n" << raw_log;
    EXPECT_GT(*in_tokens, 0);
    EXPECT_GT(*out_tokens, *thought_tokens);

    ASSERT_TRUE(fs::exists(cache_path));
    ASSERT_TRUE(fs::exists(translated_path));
    ASSERT_TRUE(fs::exists(tsv_path));

    const snt::Bytes cache_bytes_after_pass1 = snt::read_file(cache_path.u8string());
    const bj::object cache_obj = snt::json_parse_file(cache_path.u8string()).as_object();
    const bj::object doc_obj = snt::json_parse_file(translated_path.u8string()).as_object();
    const bj::array& doc_entries = doc_obj.at("script\\00_sakuranotoki_live.ast")
                                       .as_object()
                                       .at("strings")
                                       .as_array();
    ASSERT_EQ(doc_entries.size(), kSakuraNoTokiLines.size() + kPreCachedContextAfterLines.size());
    const auto tsv_rows = ReadTsvRows(tsv_path);

    std::array<std::string, kLiveBatchSize> en_raw;
    std::array<std::string, kLiveBatchSize> en_low;
    for (std::size_t i = 0; i < kSakuraNoTokiLines.size(); ++i) {
        const char* jp = kSakuraNoTokiLines[i].text;
        ASSERT_NE(cache_obj.if_contains(jp), nullptr)
            << "Missing cached translation for line #" << (i + 1) << ": " << jp;
        en_raw[i] = std::string(cache_obj.at(jp).as_string());
        en_low[i] = ToLowerAscii(en_raw[i]);

        GTEST_LOG_(INFO) << "SakuraNoToki [" << (i + 1) << "] " << jp << " => " << en_raw[i];

        EXPECT_FALSE(en_raw[i].empty()) << "Empty translation on line #" << (i + 1);
        EXPECT_FALSE(snt::is_jp(en_raw[i]))
            << "Leaked Japanese characters in line #" << (i + 1) << ": " << en_raw[i];
        EXPECT_EQ(en_raw[i].find("[Naoya]"), std::string::npos);
        EXPECT_EQ(en_raw[i].find("[Shizuru]"), std::string::npos);
        EXPECT_EQ(en_raw[i].find("[NARRATION]"), std::string::npos);
        EXPECT_EQ(doc_entries[i].as_object().at("translated").as_string(), en_raw[i]);
        ASSERT_EQ(tsv_rows.count(jp), 1U) << "Missing TSV row for line #" << (i + 1);
    }

    // Artemis ruby-split compound #1 (`碧緋` -> `Hekihi`)
    EXPECT_NE(en_low[kIdxHekihiRubyBase].find("hekihi"), std::string::npos)
        << "Artemis ruby base '碧緋' failed to use pinned 'Hekihi': "
        << en_raw[kIdxHekihiRubyBase];

    // Artemis ruby-split proper noun #2 (`円木` -> `Tsuburagi`)
    EXPECT_NE(en_low[kIdxTsuburagiRubyBase].find("tsuburagi"), std::string::npos)
        << "Artemis ruby base '円木' failed to use pinned 'Tsuburagi': "
        << en_raw[kIdxTsuburagiRubyBase];

    // Pinned artwork title (`『蝶を夢む』` -> `Dreaming of a Butterfly`)
    EXPECT_NE(en_low[kIdxButterflyTitle].find("dreaming of a butterfly"), std::string::npos)
        << "Artwork title '『蝶を夢む』' missed pinned 'Dreaming of a Butterfly': "
        << en_raw[kIdxButterflyTitle];

    // Glossary cast (`稟`=`Rin`, `圭`=`Kei`, `藍`=`Ai`) + `弓張学園` (`Yumihari`) + U+3000 indent
    EXPECT_NE(en_low[kIdxYumihariIndented].find("yumihari"), std::string::npos)
        << en_raw[kIdxYumihariIndented];
    EXPECT_NE(en_low[kIdxYumihariIndented].find("rin"), std::string::npos)
        << en_raw[kIdxYumihariIndented];
    EXPECT_NE(en_low[kIdxYumihariIndented].find("kei"), std::string::npos)
        << en_raw[kIdxYumihariIndented];
    EXPECT_NE(en_low[kIdxYumihariIndented].find("ai"), std::string::npos)
        << en_raw[kIdxYumihariIndented];
    EXPECT_EQ(tsv_rows.at(kSakuraNoTokiLines[kIdxYumihariIndented].text).rfind("　", 0), 0U)
        << "translations.tsv lost leading U+3000 paragraph indent: "
        << tsv_rows.at(kSakuraNoTokiLines[kIdxYumihariIndented].text);

    // Pass 2 resumes at production batch size and must issue zero API requests.
    const ProcessResult pass2 = RunProcessCapturingOutput(
        translate_exe,
        {"--dir", sandbox.path().u8string(),
         "--batch", std::to_string(kProductionBatchSize)});

    ASSERT_EQ(pass2.exit_code, kExpectedExitCode)
        << "Pass 2 resume failed:\n" << pass2.output;
    EXPECT_NE(pass2.output.find("uncached strings: 0"), std::string::npos) << pass2.output;
    EXPECT_NE(pass2.output.find("Everything is already cached!"), std::string::npos)
        << pass2.output;
    EXPECT_EQ(pass2.output.find("GEMINI RAW ANSWER"), std::string::npos)
        << "Pass 2 unexpectedly issued a live API request:\n" << pass2.output;
    EXPECT_EQ(pass2.output.find("[ERROR]"), std::string::npos) << pass2.output;

    const snt::Bytes raw_log_after_pass2 = snt::read_file(raw_log_path.u8string());
    EXPECT_EQ(raw_log_after_pass2, raw_log_after_pass1)
        << "gemini_raw_answers.log changed on 0-API-call resume pass";

    const snt::Bytes cache_bytes_after_pass2 = snt::read_file(cache_path.u8string());
    EXPECT_EQ(cache_bytes_after_pass2, cache_bytes_after_pass1)
        << "translation_cache_gemini.json mutated on 2nd-pass resume";

    ASSERT_TRUE(fs::exists(coverage_path));
    const bj::object cov_totals = snt::json_parse_file(coverage_path.u8string())
                                      .as_object()
                                      .at("totals")
                                      .as_object();
    EXPECT_EQ(cov_totals.at("identity").as_int64(), 0)
        << "Untranslated identity lines remained after complete pass";

    const auto tsv_rows_pass2 = ReadTsvRows(tsv_path);
    EXPECT_EQ(tsv_rows_pass2, tsv_rows);
}
