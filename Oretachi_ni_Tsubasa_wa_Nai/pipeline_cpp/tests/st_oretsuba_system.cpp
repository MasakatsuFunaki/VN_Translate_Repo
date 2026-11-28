// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include "apps/paths.h"
#include "common/util.h"
#include "extract/extractor.h"
#include "lpk/lpk_archive.h"
#include "sob/sob_parser.h"
#include "translate/gemini_client.h"
#include "translate/glossary.h"
#include "translate/speaker_gate.h"
#include "translate/translate_core.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace bj = boost::json;

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view prefix) {
        static std::atomic<unsigned int> seq{0U};
        const unsigned int id = seq.fetch_add(1U, std::memory_order_relaxed);
        const std::string dir_name = std::string(prefix) + "_" +
                                     std::to_string(::GetCurrentProcessId()) + "_" +
                                     std::to_string(id);
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

constexpr std::size_t kExpectedArchiveEntries = 418U;
constexpr std::size_t kExpectedSobFiles = 413U;
constexpr std::size_t kExpectedNonSobFileCount = 5U;
constexpr std::size_t kSobMagicLength = 4U;
constexpr std::size_t kExpectedSob0Count = 411U;
constexpr std::size_t kExpectedSob1Count = 1U;
constexpr std::size_t kExpectedSob2Count = 1U;
constexpr std::size_t kExpectedScenarioScripts = 387U;
constexpr std::size_t kExpectedMessageCount = 57865U;
constexpr std::size_t kExpectedVoicedDialogueCount = 21424U;
constexpr std::size_t kExpectedUnvoicedDialogueCount = 13744U;
constexpr std::size_t kExpectedVoiceOnlyDialogueCount = 304U;
constexpr std::size_t kExpectedNarrationCount = 22393U;
constexpr std::size_t kExpectedChoiceBlockCount = 24U;
constexpr std::size_t kExpectedChoiceOptionCount = 54U;
constexpr std::size_t kExpectedTitleCount = 388U;
constexpr std::size_t kExpectedSynopsisCount = 388U;
constexpr std::size_t kExpectedUiStringCount = 156U;
constexpr std::size_t kExpectedTotalEntryCount = 58851U;
constexpr std::size_t kExpectedUniqueSpeakerCount = 324U;
constexpr std::size_t kMinExpectedUniqueJpStrings = 51914U;
constexpr std::size_t kExpectedUserVarUiRecords = 10U;
constexpr std::size_t kExpectedS0102cChoices = 2U;

constexpr std::size_t kMinCorpusTranslatableLines = 1000U;
constexpr std::size_t kEarlyNovelAnchorOffset = 100U;
constexpr std::size_t kMidNovelDivisor = 2U;
constexpr std::size_t kLateNovelTailOffset = 200U;
constexpr std::size_t kMovingWindowSize = 8U;
constexpr std::size_t kJudgeMaxOutputTokens = 32768U;
constexpr DWORD kCliProcessTimeoutMs = 660000U;
constexpr double kMaxLiveBatchLatencySeconds =
    static_cast<double>(vn::llm::kDefaultReadTimeoutSeconds);
constexpr std::int64_t kMinCalibratedThoughtTokensFloor = 64;
constexpr DWORD kProcessSuccessExitCode = 0U;
constexpr DWORD kProcessFailureExitCode = 1U;
constexpr std::int64_t kMissingMetricSentinel = -1;
constexpr std::size_t kPipeReadBufferBytes = 4096U;
constexpr std::size_t kSandboxScriptLineCount =
    static_cast<std::size_t>(oretsuba::translate::kDefaultBatchSize);
constexpr std::size_t kExpectedKeywaitPauseRows = 3U;

class ScopedHandle {
public:
    ScopedHandle() noexcept = default;
    explicit ScopedHandle(HANDLE h) noexcept : handle_(h) {}
    ~ScopedHandle() { Reset(); }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    HANDLE get() const noexcept { return handle_; }

    void Reset(HANDLE next = nullptr) noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle_);
        }
        handle_ = next;
    }

private:
    HANDLE handle_ = nullptr;
};

struct ProcessResult {
    bool completed = false;
    DWORD exit_code = kProcessFailureExitCode;
    double elapsed_seconds = 0.0;
    std::string combined_output;
};

std::wstring QuoteCommandLineArg(std::wstring_view arg) {
    std::wstring out;
    out.push_back(L'"');
    for (const wchar_t ch : arg) {
        if (ch == L'"') {
            out.push_back(L'\\');
        }
        out.push_back(ch);
    }
    out.push_back(L'"');
    return out;
}

ProcessResult RunChildProcessCapture(const fs::path& exe_path,
                                     const std::vector<std::wstring>& args,
                                     const fs::path& working_dir,
                                     DWORD timeout_ms = kCliProcessTimeoutMs) {
    ProcessResult result;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE raw_read = nullptr;
    HANDLE raw_write = nullptr;
    if (!::CreatePipe(&raw_read, &raw_write, &sa, 0)) {
        result.combined_output = "CreatePipe failed";
        return result;
    }
    ScopedHandle read_pipe(raw_read);
    ScopedHandle write_pipe(raw_write);
    ::SetHandleInformation(read_pipe.get(), HANDLE_FLAG_INHERIT, 0);

    std::wstring cmd_line = QuoteCommandLineArg(exe_path.wstring());
    for (const auto& arg : args) {
        cmd_line.push_back(L' ');
        cmd_line += QuoteCommandLineArg(arg);
    }
    std::vector<wchar_t> mutable_cmd(cmd_line.begin(), cmd_line.end());
    mutable_cmd.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = write_pipe.get();
    si.hStdError = write_pipe.get();

    PROCESS_INFORMATION pi{};
    const std::wstring cwd_wide = working_dir.wstring();
    const auto start_time = std::chrono::steady_clock::now();
    const BOOL created = ::CreateProcessW(
        exe_path.wstring().c_str(),
        mutable_cmd.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        cwd_wide.c_str(),
        &si,
        &pi);
    write_pipe.Reset();
    if (!created) {
        result.combined_output =
            "CreateProcessW failed with error " + std::to_string(::GetLastError());
        return result;
    }

    ScopedHandle proc_handle(pi.hProcess);
    ScopedHandle thread_handle(pi.hThread);

    std::thread reader([&]() {
        char buffer[kPipeReadBufferBytes];
        DWORD bytes_read = 0;
        while (::ReadFile(
                   read_pipe.get(),
                   buffer,
                   static_cast<DWORD>(sizeof(buffer)),
                   &bytes_read,
                   nullptr) &&
               bytes_read > 0) {
            result.combined_output.append(buffer, bytes_read);
        }
    });

    const DWORD wait_status = ::WaitForSingleObject(proc_handle.get(), timeout_ms);
    if (wait_status == WAIT_OBJECT_0) {
        result.completed = true;
    } else {
        ::TerminateProcess(proc_handle.get(), kProcessFailureExitCode);
        ::WaitForSingleObject(proc_handle.get(), INFINITE);
    }
    reader.join();
    result.elapsed_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
    ::GetExitCodeProcess(proc_handle.get(), &result.exit_code);
    return result;
}

fs::path FindBuiltPipelineExe(const fs::path& proj_dir, std::string_view exe_name) {
    std::error_code ec;
    const std::string self_dir_str = oretsuba::apps::ExecutableDirectory();
    if (!self_dir_str.empty()) {
        const fs::path self_dir = fs::u8path(self_dir_str);
        const fs::path sibling_cfg =
            self_dir.parent_path().parent_path() / self_dir.filename() / fs::u8path(exe_name);
        if (fs::is_regular_file(sibling_cfg, ec)) {
            return sibling_cfg;
        }
        const fs::path same_dir = self_dir / fs::u8path(exe_name);
        if (fs::is_regular_file(same_dir, ec)) {
            return same_dir;
        }
    }
    const fs::path staged = proj_dir / "build" / "install" / "bin" / fs::u8path(exe_name);
    if (fs::is_regular_file(staged, ec)) {
        return staged;
    }
    return {};
}

std::string AsciiLower(std::string_view input) {
    std::string out(input);
    for (char& ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

bool ContainsAnySubstring(std::string_view haystack,
                          std::initializer_list<std::string_view> needles) {
    return std::any_of(needles.begin(), needles.end(), [&](std::string_view needle) {
        return haystack.find(needle) != std::string_view::npos;
    });
}

bool IsCp932LosslessRoundTrip(std::string_view utf8) {
    const oretsuba::Bytes cp932 = oretsuba::Utf8ToCp932(utf8);
    const std::optional<std::string> round_tripped = oretsuba::Cp932ToUtf8Strict(cp932);
    return round_tripped.has_value() && *round_tripped == utf8;
}

std::int64_t ExtractThoughtsTokensFromTelemetryLog(std::string_view log_text) {
    constexpr std::string_view kTelemetryLineMarker = "Tokens: in=";
    constexpr std::string_view kMarker = "thoughts=";
    const std::size_t line_pos = log_text.rfind(kTelemetryLineMarker);
    if (line_pos == std::string_view::npos) {
        return kMissingMetricSentinel;
    }
    const std::size_t pos = log_text.find(kMarker, line_pos);
    if (pos == std::string_view::npos) {
        return kMissingMetricSentinel;
    }
    std::size_t cursor = pos + kMarker.size();
    if (cursor >= log_text.size() ||
        !std::isdigit(static_cast<unsigned char>(log_text[cursor]))) {
        return kMissingMetricSentinel;
    }
    std::int64_t value = 0;
    constexpr std::int64_t kDecimalRadix = 10;
    while (cursor < log_text.size() &&
           std::isdigit(static_cast<unsigned char>(log_text[cursor]))) {
        value = value * kDecimalRadix + (log_text[cursor] - '0');
        ++cursor;
    }
    return value;
}

std::string ReadWideEnvUtf8(const wchar_t* var_name) {
    const wchar_t* wide_val = ::_wgetenv(var_name);
    if (wide_val == nullptr || *wide_val == L'\0') {
        return {};
    }
    const std::wstring_view wide_view(wide_val);
    const int required = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide_view.data(), static_cast<int>(wide_view.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(required), '\0');
    const int written = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, wide_view.data(), static_cast<int>(wide_view.size()),
        utf8.data(), required, nullptr, nullptr);
    if (written != required) {
        return {};
    }
    return utf8;
}

std::string FindScriptLpkPath() {
    const std::string env_dir = ReadWideEnvUtf8(L"VN_GAME_DIR");
    if (env_dir.empty()) {
        return {};
    }
    std::error_code ec;
    const fs::path base = fs::u8path(env_dir);
    if (fs::is_regular_file(base, ec)) {
        return base.u8string();
    }
    const fs::path candidate = base / "SCRIPT.LPK";
    if (fs::is_regular_file(candidate, ec)) {
        return candidate.u8string();
    }
    return {};
}

}  // namespace

TEST(LpkArchiveTest, UnpacksRealScriptLpkAndVerifiesSobMagics) {
    const std::string lpk_path = FindScriptLpkPath();
    if (lpk_path.empty()) {
        GTEST_SKIP() << "VN_GAME_DIR unset or SCRIPT.LPK not found";
    }

    const oretsuba::lpk::LpkArchiveReader reader(lpk_path);
    EXPECT_EQ(reader.EntryCount(), kExpectedArchiveEntries);

    std::size_t sob_count = 0;
    std::size_t non_sob_count = 0;
    for (const auto& entry : reader.Entries()) {
        if (entry.name.size() >= kSobMagicLength &&
            entry.name.substr(entry.name.size() - kSobMagicLength) == ".sob") {
            ++sob_count;
            continue;
        }
        ++non_sob_count;
    }
    EXPECT_EQ(sob_count, kExpectedSobFiles);
    EXPECT_EQ(non_sob_count, kExpectedNonSobFileCount);

    const oretsuba::Bytes main_sob = reader.ReadEntryByName("main.sob");
    ASSERT_GE(main_sob.size(), kSobMagicLength);
    EXPECT_EQ(std::memcmp(main_sob.data(), "SOB0", kSobMagicLength), 0);

    const oretsuba::Bytes s01_sob = reader.ReadEntryByName("s01_01a.sob");
    ASSERT_GE(s01_sob.size(), kSobMagicLength);
    EXPECT_EQ(std::memcmp(s01_sob.data(), "SOB0", kSobMagicLength), 0);

    const oretsuba::Bytes user_var = reader.ReadEntryByName("user_var.sob");
    ASSERT_GE(user_var.size(), kSobMagicLength);
    EXPECT_EQ(std::memcmp(user_var.data(), "SOB1", kSobMagicLength), 0);

    const oretsuba::Bytes func_lib = reader.ReadEntryByName("func_lib.sob");
    ASSERT_GE(func_lib.size(), kSobMagicLength);
    EXPECT_EQ(std::memcmp(func_lib.data(), "SOB2", kSobMagicLength), 0);
}

TEST(SobParserTest, ParsesRealScenarioAndUserVarScriptsFromScriptLpk) {
    const std::string lpk_path = FindScriptLpkPath();
    if (lpk_path.empty()) {
        GTEST_SKIP() << "VN_GAME_DIR unset or SCRIPT.LPK not found";
    }

    const oretsuba::lpk::LpkArchiveReader reader(lpk_path);
    const oretsuba::sob::SobScriptParser parser;

    const auto s01_02c = parser.Parse("s01_02c.sob", reader.ReadEntryByName("s01_02c.sob"));
    EXPECT_EQ(s01_02c.format, oretsuba::sob::SobFormat::kSob0);
    EXPECT_TRUE(s01_02c.has_messages);
    EXPECT_EQ(s01_02c.choice_block_count, 1U);

    std::vector<std::uint16_t> msg_indices;
    std::size_t choice_count = 0;
    for (const auto& rec : s01_02c.records) {
        if (rec.type == oretsuba::sob::SobEntryType::kDialogue ||
            rec.type == oretsuba::sob::SobEntryType::kNarration) {
            msg_indices.push_back(rec.record_index);
        }
        if (rec.type == oretsuba::sob::SobEntryType::kChoice) {
            ++choice_count;
        }
    }
    EXPECT_EQ(choice_count, kExpectedS0102cChoices);
    ASSERT_FALSE(msg_indices.empty());
    EXPECT_EQ(msg_indices.front(), 0U);
    for (std::size_t i = 1U; i < msg_indices.size(); ++i) {
        EXPECT_GT(msg_indices[i], msg_indices[i - 1U]);
    }

    const auto user_var = parser.Parse("user_var.sob", reader.ReadEntryByName("user_var.sob"));
    EXPECT_EQ(user_var.format, oretsuba::sob::SobFormat::kSob1);
    ASSERT_EQ(user_var.records.size(), kExpectedUserVarUiRecords);
    EXPECT_EQ(user_var.records.front().type, oretsuba::sob::SobEntryType::kUi);
    EXPECT_EQ(user_var.records.front().text, "クイックセーブ");
}

TEST(ExtractorTest, ExtractsAllScriptLpkStringsWithExactCorpusCountsAndZeroReplacementChars) {
    const std::string lpk_path = FindScriptLpkPath();
    if (lpk_path.empty()) {
        GTEST_SKIP() << "VN_GAME_DIR unset or SCRIPT.LPK not found";
    }

    const ScopedTempDir temp_dir("oretsuba_st_extract");
    const fs::path temp_json = temp_dir.path() / "extracted_text.json";

    const oretsuba::extract::Extractor extractor;
    const auto summary = extractor.ExtractArchive(lpk_path, temp_json.u8string());

    EXPECT_EQ(summary.total_archive_entries, kExpectedArchiveEntries);
    EXPECT_EQ(summary.sob_file_count, kExpectedSobFiles);
    EXPECT_EQ(summary.sob0_count, kExpectedSob0Count);
    EXPECT_EQ(summary.sob1_count, kExpectedSob1Count);
    EXPECT_EQ(summary.sob2_count, kExpectedSob2Count);
    EXPECT_EQ(summary.scenario_script_count, kExpectedScenarioScripts);
    EXPECT_EQ(summary.message_count, kExpectedMessageCount);
    EXPECT_EQ(summary.voiced_dialogue_count, kExpectedVoicedDialogueCount);
    EXPECT_EQ(summary.unvoiced_dialogue_count, kExpectedUnvoicedDialogueCount);
    EXPECT_EQ(summary.voice_only_dialogue_count, kExpectedVoiceOnlyDialogueCount);
    EXPECT_EQ(summary.narration_count, kExpectedNarrationCount);
    EXPECT_EQ(summary.choice_block_count, kExpectedChoiceBlockCount);
    EXPECT_EQ(summary.choice_option_count, kExpectedChoiceOptionCount);
    EXPECT_EQ(summary.title_count, kExpectedTitleCount);
    EXPECT_EQ(summary.synopsis_count, kExpectedSynopsisCount);
    EXPECT_EQ(summary.ui_string_count, kExpectedUiStringCount);
    EXPECT_EQ(summary.total_entry_count, kExpectedTotalEntryCount);
    EXPECT_EQ(summary.unique_speaker_count, kExpectedUniqueSpeakerCount);
    EXPECT_GE(summary.unique_jp_string_count, kMinExpectedUniqueJpStrings);
    EXPECT_EQ(summary.replacement_char_count, 0U);

    ASSERT_TRUE(fs::is_regular_file(temp_json));
    const bj::value root_val = oretsuba::JsonParseFile(temp_json.u8string());
    ASSERT_TRUE(root_val.is_object());
    const auto& root_obj = root_val.as_object();
    ASSERT_FALSE(root_obj.empty());
    EXPECT_EQ(root_obj.begin()->key(), "s00_00.sob");
    ASSERT_NE(root_obj.if_contains("s01_01a.sob"), nullptr);
    ASSERT_NE(root_obj.if_contains("user_var.sob"), nullptr);
}

TEST(TranslateSystemTest, SpeakerGatePassesOnLocalTranslatedAndExtractedCorpora) {
    using namespace oretsuba::translate;

    const fs::path proj_dir = fs::u8path(ORETSUBA_PROJECT_DIR);
    const fs::path tone_path = proj_dir / "TONE_OF_NOVEL.md";
    const fs::path translated_path = proj_dir / "script_output" / "translated_text.json";
    const fs::path extracted_path = proj_dir / "script_output" / "extracted_text.json";

    const bool has_translated = fs::is_regular_file(translated_path);
    const bool has_extracted = fs::is_regular_file(extracted_path);
    if (!has_translated && !has_extracted) {
        GTEST_SKIP() << "Local script_output corpora not present on disk";
    }

    ASSERT_TRUE(fs::is_regular_file(tone_path)) << tone_path.u8string();
    Glossary glossary;
    ASSERT_TRUE(glossary.LoadFromToneFile(tone_path.u8string()));

    SpeakerGate gate;
    constexpr std::size_t kExpectedFullCorpusSpeakers = 323U;

    if (has_translated) {
        const SpeakerCheckReport report =
            gate.ValidateExtractedFile(translated_path.u8string(), glossary);
        EXPECT_TRUE(report.passed) << report.reason;
        EXPECT_EQ(report.corpus_speakers, kExpectedFullCorpusSpeakers);
    }
    if (has_extracted) {
        const SpeakerCheckReport report =
            gate.ValidateExtractedFile(extracted_path.u8string(), glossary);
        EXPECT_TRUE(report.passed) << report.reason;
        EXPECT_EQ(report.corpus_speakers, kExpectedFullCorpusSpeakers);
    }
}

TEST(TranslateSystemTest, MovingWindowCarbonJudgeAuditAcrossThreeNovelWindows) {
    const wchar_t* live_api_env = ::_wgetenv(L"VN_RUN_LIVE_API_TESTS");
    if (live_api_env == nullptr ||
        std::wstring_view(live_api_env) != std::wstring_view(L"1")) {
        GTEST_SKIP() << "VN_RUN_LIVE_API_TESTS!=1; skipping live LLM API system test";
    }

    using namespace oretsuba::translate;

    const fs::path proj_dir = fs::u8path(ORETSUBA_PROJECT_DIR);
    const fs::path extracted_path = proj_dir / "script_output" / "extracted_text.json";
    if (!fs::is_regular_file(extracted_path)) {
        GTEST_SKIP() << "extracted_text.json not found";
    }

    const std::string api_key = LoadGeminiApiKey(proj_dir.u8string());
    if (api_key.empty()) {
        GTEST_SKIP() << "GEMINI_API_KEY not configured";
    }

    const bj::value root = oretsuba::JsonParseFile(extracted_path.u8string());
    ASSERT_TRUE(root.is_object());

    Glossary glossary((proj_dir / "TONE_OF_NOVEL.md").u8string());
    SpeakerGate gate;

    struct WindowSample {
        std::string label;
        std::vector<std::string> jp_raw;
        std::vector<std::string> prompt_lines;
        std::vector<std::string> prev_context;
        std::vector<std::string> context_after;
    };

    std::vector<std::pair<std::string, std::string>> all_lines;
    for (const auto& kv : root.as_object()) {
        if (!kv.value().is_object()) {
            continue;
        }
        const auto* strings_v = kv.value().as_object().if_contains("strings");
        if (!strings_v || !strings_v->is_array()) {
            continue;
        }
        for (const auto& item : strings_v->as_array()) {
            if (!item.is_object()) {
                continue;
            }
            const auto& obj = item.as_object();
            const auto* txt = obj.if_contains("text");
            if (!txt || !txt->is_string()) {
                continue;
            }
            const std::string jp(txt->as_string());
            if (!NeedsTranslation(jp)) {
                continue;
            }
            std::string spk;
            if (const auto* s = obj.if_contains("speaker"); s && s->is_string()) {
                spk = std::string(s->as_string());
            }
            all_lines.emplace_back(spk, jp);
        }
    }
    ASSERT_GT(all_lines.size(), kMinCorpusTranslatableLines);

    const std::vector<std::pair<std::string, std::size_t>> anchors = {
        {"early_novel", kEarlyNovelAnchorOffset},
        {"mid_novel", all_lines.size() / kMidNovelDivisor},
        {"late_novel", all_lines.size() - kLateNovelTailOffset},
    };

    auto format_line = [&](const std::pair<std::string, std::string>& item) {
        const std::string clean_jp = gate.DecomposeInlineControls(item.second).clean_text;
        const auto spk_en = glossary.LookupSpeaker(item.first);
        if (spk_en.has_value() && !spk_en->empty()) {
            return "[" + *spk_en + "] " + clean_jp;
        }
        return "[NARRATION] " + clean_jp;
    };

    const fs::path cache_path = proj_dir / "script_output" / "translation_cache_gemini.json";
    TranslationCache disk_cache = LoadCache(cache_path.u8string());
    bool cache_updated = false;

    GeminiClient client(api_key, kDefaultModel, proj_dir.u8string());
    std::ostringstream judge_payload;

    for (const auto& [label, start_idx] : anchors) {
        WindowSample win;
        win.label = label;
        const std::size_t ctx_begin =
            (start_idx > kContextWindowLines) ? (start_idx - kContextWindowLines) : 0U;
        for (std::size_t i = ctx_begin; i < start_idx; ++i) {
            win.prev_context.push_back(format_line(all_lines[i]));
        }
        std::map<int, std::string> cached_results;
        for (std::size_t i = start_idx; i < start_idx + kMovingWindowSize; ++i) {
            win.jp_raw.push_back(all_lines[i].second);
            win.prompt_lines.push_back(format_line(all_lines[i]));
            const std::string clean_jp =
                gate.DecomposeInlineControls(all_lines[i].second).clean_text;
            if (const auto* hit = disk_cache.Find(clean_jp)) {
                cached_results[static_cast<int>((i - start_idx) + 1U)] = *hit;
            }
        }
        for (std::size_t i = start_idx + kMovingWindowSize;
             i < std::min(all_lines.size(), start_idx + kMovingWindowSize + kContextAfterLines);
             ++i) {
            win.context_after.push_back(format_line(all_lines[i]));
        }

        const bool was_cached = (cached_results.size() == kMovingWindowSize);
        const auto results = was_cached
            ? cached_results
            : TranslateBatchWithFallback(
                  client, win.prompt_lines, win.prev_context, win.context_after);
        ASSERT_EQ(results.size(), kMovingWindowSize) << "Window failed: " << label;

        judge_payload << "Window: " << label << "\n";
        for (std::size_t i = 0; i < kMovingWindowSize; ++i) {
            const std::string en =
                SanitizeAscii(StripSpeakerTags(results.at(static_cast<int>(i + 1U))));
            EXPECT_FALSE(en.empty());
            EXPECT_FALSE(NeedsTranslation(en)) << "Residual JP in: " << en;
            if (!was_cached) {
                const std::string clean_jp =
                    gate.DecomposeInlineControls(win.jp_raw[i]).clean_text;
                disk_cache.Set(clean_jp, en);
                cache_updated = true;
            }
            judge_payload << (i + 1U) << ". JP: " << win.prompt_lines[i] << "\n"
                          << "   EN: " << en << "\n";
        }
        judge_payload << "\n";
    }
    if (cache_updated && fs::is_regular_file(cache_path)) {
        SaveCache(cache_path.u8string(), disk_cache);
    }

    bj::object judge_req;
    judge_req["model"] = kDefaultModel;
    judge_req["max_tokens"] = static_cast<std::int64_t>(kJudgeMaxOutputTokens);
    judge_req["system"] =
        "You are a senior Japanese-to-English visual novel localization quality auditor. "
        "Verify whether the English translations across all three windows accurately and naturally "
        "translate the Japanese source lines from Oretachi ni Tsubasa wa Nai without omissions, "
        "untranslated Japanese, or leaked [Speaker] tags. End your response with 'VERDICT: PASS' "
        "if all three windows meet high literary localization quality, or 'VERDICT: FAIL' otherwise.";
    bj::array messages;
    bj::object user_msg;
    user_msg["role"] = "user";
    user_msg["content"] = judge_payload.str();
    messages.push_back(std::move(user_msg));
    judge_req["messages"] = std::move(messages);

    const bj::value judge_resp = client.Messages(judge_req);
    ASSERT_TRUE(judge_resp.is_object());
    std::string judge_text;
    if (const auto* content_v = judge_resp.as_object().if_contains("content");
        content_v && content_v->is_array()) {
        for (const auto& block : content_v->as_array()) {
            if (!block.is_object()) {
                continue;
            }
            if (const auto* txt = block.as_object().if_contains("text");
                txt && txt->is_string()) {
                judge_text += std::string(txt->as_string());
            }
        }
    }
    std::cout << "[Carbon Judge Report]\n" << judge_text << "\n";
    EXPECT_NE(judge_text.find("VERDICT: PASS"), std::string::npos)
        << "Judge did not return VERDICT: PASS:\n" << judge_text;
}

TEST(TranslateSystemTest, LiveOretsubaBatchPrecheckControlCodesCp932AndResumeIdempotency) {
    using namespace oretsuba::translate;

    const fs::path proj_dir = fs::u8path(ORETSUBA_PROJECT_DIR);
    const std::string api_key = LoadGeminiApiKey(proj_dir.u8string());
    if (api_key.empty()) {
        if (ReadWideEnvUtf8(L"GITHUB_ACTIONS") == "true") {
            GTEST_SKIP()
                << "GITHUB_ACTIONS=true and GEMINI_API_KEY is absent; skipping live Oretsuba system test";
        }
        FAIL() << "GEMINI_API_KEY is missing locally; configure GEMINI_API_KEY in environment, "
                  "~/.gemini/.env, HKCU\\Environment, or .claude/settings.json";
    }

    const fs::path tone_src = proj_dir / "TONE_OF_NOVEL.md";
    ASSERT_TRUE(fs::is_regular_file(tone_src)) << "Missing TONE_OF_NOVEL.md: " << tone_src.u8string();

    const fs::path built_translate_exe = FindBuiltPipelineExe(proj_dir, "02_translate.exe");
    ASSERT_FALSE(built_translate_exe.empty()) << "Built 02_translate.exe not found";
    const fs::path built_tsv_exe = FindBuiltPipelineExe(proj_dir, "03_build_tsv.exe");
    ASSERT_FALSE(built_tsv_exe.empty()) << "Built 03_build_tsv.exe not found";

    const ScopedTempDir sandbox("oretsuba_st_tier2_live");
    const fs::path sandbox_bin = sandbox.path() / "bin";
    const fs::path sandbox_script_out = sandbox.path() / "script_output";
    const fs::path sandbox_game = sandbox.path() / "game";
    fs::create_directories(sandbox_bin);
    fs::create_directories(sandbox_script_out);
    fs::create_directories(sandbox_game);

    const fs::path staged_translate_exe = sandbox_bin / "02_translate.exe";
    const fs::path staged_tsv_exe = sandbox_bin / "03_build_tsv.exe";
    fs::copy_file(built_translate_exe, staged_translate_exe, fs::copy_options::overwrite_existing);
    fs::copy_file(built_tsv_exe, staged_tsv_exe, fs::copy_options::overwrite_existing);
    fs::copy_file(tone_src, sandbox.path() / "TONE_OF_NOVEL.md", fs::copy_options::overwrite_existing);

    constexpr const char* kJpTakashiIntro =
        "　僕の名前は羽田鷹志。私立美空学園の三年生。";
    constexpr const char* kJpTakashiFeathers =
        "　いまの僕──つまり羽田鷹志は、グレタガルドの先代国王が集めた次期王候補“万色の四枚羽根”のひとり、$&聖騎士ホークの≪転生≫した姿だ。";
    constexpr const char* kJpTakashiFeathersNormalizedKey =
        "いまの僕──つまり羽田鷹志は、グレタガルドの先代国王が集めた次期王候補“万色の四枚羽根”のひとり、$&聖騎士ホークの≪転生≫した姿だ。";
    constexpr const char* kJpKobatoRaw =
        "<10>\\「お兄ちゃん、<\"柳原|やなぎはら\">の駅前まで一緒に行こうよ！$&遅れちゃうよ！」";
    constexpr const char* kJpKobatoNormalizedKey =
        "「お兄ちゃん、柳原の駅前まで一緒に行こうよ！$&遅れちゃうよ！」";
    constexpr const char* kJpAsukaPause =
        "「なに？$&アンタ、さっきから私の顔をジロジロ見てるけど、何か用なの？」";
    constexpr const char* kJpHawkChivalry =
        "「案ずるな、我が忠実なる騎士ペガサスよ！　王女アスカとグレタガルドの誇りは、この聖騎士ホークが必ずや守り抜く！」";

    struct SandboxScriptEntry {
        const char* type;
        const char* speaker;
        const char* speaker_en;
        const char* text;
    };

    static constexpr SandboxScriptEntry kSandboxEntries[] = {
        {"dialogue", "タカシ", "Takashi", kJpTakashiIntro},
        {"dialogue", "鷹志", "Takashi", kJpTakashiFeathers},
        {"dialogue", "小鳩", "Kobato", kJpKobatoRaw},
        {"dialogue", "明日香", "Asuka", kJpAsukaPause},
        {"dialogue", "聖騎士ホーク", "Holy Knight Hawk", kJpHawkChivalry},
        {"dialogue",
         "騎士ペガサス",
         "Knight Pegasus",
         "「おおっ、さすがはサー・ホーク！　ベルツモンド修道院の鐘が鳴り響いておりますぞ！」"},
        {"dialogue",
         "王女アスカ",
         "Princess Asuka",
         "「ふん、口先だけなら何とでも言えるわ。ちゃんと結果で示しなさいよね、ホーク卿！」"},
        {"dialogue",
         "ガルーダ",
         "Garuda",
         "「忘れるなよ、ホーク。異世界の扉が開く刻限は、すでに目前まで迫っているのだ」"},
        {"narration",
         "",
         "",
         "　冬の柳原を吹き抜ける冷たい朝風で、僕の妄想はあっけなく現実の教室へと引き戻された。"},
        {"dialogue",
         "京",
         "Kyo",
         "「あ、あの……羽田くん、おはようございます。今日もなんだか寒そうですね……」"},
        {"dialogue",
         "和馬",
         "Kazuma",
         "「よおタカシ！　朝からぼーっとしてんじゃねえぞ、高内先生のホームルームが始まるぜ！」"},
        {"dialogue",
         "高内",
         "Takauchi",
         "「こら森里、席に着きなさい！　Ｈ組は朝から騒がしくてかなわんわね」"},
        {"dialogue",
         "鷹志",
         "Takashi",
         "「す、すみません高内先生……ちょっと考えごとをしていただけで」"},
        {"dialogue",
         "明日香",
         "Asuka",
         "「はあ……アンタのその寝ぼけた顔を見てると、こっちまで調子が狂うのよ」"},
        {"dialogue",
         "小鳩",
         "Kobato",
         "「お兄ちゃん、放課後は柳原の通りで焼きたてのクレープを食べて帰ろうね！」"},
        {"dialogue",
         "春恵",
         "Harue",
         "「おう、いらっしゃい小鳩ちゃん！　特製チョコバナナクレープ、たっぷり巻いといたよ！」"},
        {"narration",
         "",
         "",
         "　夕暮れのネオンが灯り始める頃、喫茶店アレキサンドリアの扉が軽やかな鈴の音を立てた。"},
        {"dialogue",
         "鷲介",
         "Shuusuke",
         "「おっす、日和子ちゃん！　今日もアレキサンドリアの看板娘は真面目に働いてるねえ」"},
        {"dialogue",
         "日和子",
         "Hiyoko",
         "「ち、千歳さん！　看板娘だなんてからかわないでください、まだ仕込みの最中なんですから！」"},
        {"dialogue",
         "英里子",
         "Eriko",
         "「ちょっと鷲介、油売ってないでさっさとテーブル拭いてきなさいよ。時給泥棒になるわよ」"},
        {"dialogue",
         "紀奈子",
         "Kinako",
         "「まあまあ英里子ちゃん、温かい紅茶を淹れましたから、みんなでひと息つきましょうねぇ」"},
        {"dialogue",
         "狩男",
         "Karuo",
         "「ふっふっふ、紀奈子くんの優しさはまさに砂漠のオアシスだな！　マスターも感激だぞ！」"},
        {"dialogue",
         "鷲介",
         "Shuusuke",
         "「マスターはサボる口実を見つけただけだろっての。ほら、客席のオーダー入ってるぜ」"},
        {"dialogue",
         "蔵人",
         "Kurando",
         "「ふん、相変わらず騒々しい店だな。少しは静かに珈琲を味わわせてくれないものか」"},
        {"dialogue",
         "日和子",
         "Hiyoko",
         "「あ、針生さん……！　あの、先日の原稿の感想、もしよろしければ聞かせていただけますか？」"},
        {"dialogue",
         "優",
         "Yu",
         "「日和子ちゃんの新しいプロット、編集部の会議でもなかなか評判が良かったわよ」"},
        {"dialogue",
         "鷲介",
         "Shuusuke",
         "「おおっ、すげえじゃん玉泉！　これは今夜、お祝いにパレートの法則でも語っちゃうか？」"},
        {"dialogue",
         "英里子",
         "Eriko",
         "「アンタが語るとロクな話にならないから却下。ほら、さっさと皿洗い手伝いなさい」"},
        {"narration",
         "",
         "",
         "　やがて終電を過ぎた柳原の裏路地に、昼間とはまるで違う凍てついた夜の空気が降りてくる。"},
        {"dialogue",
         "隼人",
         "Hayato",
         "「……ちっ、今夜もずいぶん冷え込むな。こんな時間まで路地裏をうろつくんじゃねえよ」"},
        {"dialogue",
         "鳴",
         "Naru",
         "「あーっ、ドラだ！　ねえねえドラ、わたしの盗まれたチャリ、どこに行ったか知らない？」"},
        {"dialogue",
         "隼人",
         "Hayato",
         "「知るかよ。俺は便利屋であって、お前の自転車専門の探偵じゃねえんだ」"},
        {"dialogue",
         "亜衣",
         "Ai",
         "「あはは、鳴ちゃん相変わらず元気だねー！　成田くんもなんだかんだ言って付き合ってあげてるし」"},
        {"dialogue",
         "翔",
         "Kakeru",
         "「やれやれ、鳴が迷惑をかけたみたいだね。夜の柳原はウィングロード連中も騒いでいるから気をつけなよ」"},
        {"dialogue",
         "伽楼羅",
         "Karura",
         "「くす、夜の鳥たちが羽ばたくには、これくらい凍える風のほうがちょうど良いのやもしれぬな」"},
        {"dialogue",
         "隼人",
         "Hayato",
         "「どいつもこいつも勝手なことばっかり言いやがって……ほら鳴、風邪ひく前に帰るぞ」"},
        {"dialogue",
         "鳴",
         "Naru",
         "「えへへ、ドラがそう言うなら、もうちょっとだけ一緒に夜道を歩いてあげる！」"},
        {"narration",
         "",
         "",
         "　街灯の下に伸びる二つの影を見送りながら、冬の長い夜は静かに更けていった。"},
        {"dialogue",
         "鷹志",
         "Takashi",
         "「……あれ？　いま一瞬、どこかの誰かの夢を見ていたような気がするんだけど」"},
        {"dialogue",
         "小鳩",
         "Kobato",
         "「もう、お兄ちゃんったらまた寝ぼけてるの？　早く朝ごはん食べないと美空学園に遅刻しちゃうよ！」"},
    };
    static_assert(std::size(kSandboxEntries) == kSandboxScriptLineCount,
                  "Tier-2 live ST must stage exactly kDefaultBatchSize (40) lines");

    bj::array script_entries;
    for (const auto& item : kSandboxEntries) {
        bj::object entry{
            {"type", item.type},
            {"text", item.text},
        };
        if (item.speaker[0] != '\0') {
            entry["speaker"] = item.speaker;
        }
        script_entries.push_back(std::move(entry));
    }

    bj::object s01_obj;
    s01_obj["file"] = "s01_01a.sob";
    s01_obj["strings"] = std::move(script_entries);
    bj::object extracted_root;
    extracted_root["s01_01a.sob"] = std::move(s01_obj);

    oretsuba::WriteFileBytes(
        (sandbox_script_out / "extracted_text.json").u8string(),
        oretsuba::JsonPretty(extracted_root) + "\n");

    const std::wstring batch_arg = std::to_wstring(kDefaultBatchSize);
    const ProcessResult pass1_res = RunChildProcessCapture(
        staged_translate_exe,
        {L"--dir", sandbox.path().wstring(), L"--batch", batch_arg, L"--max-batches", L"1"},
        sandbox.path());
    std::cout << "[02_translate.exe Pass 1 (" << pass1_res.elapsed_seconds << "s)]\n"
              << pass1_res.combined_output << "\n";
    ASSERT_TRUE(pass1_res.completed) << "02_translate.exe Pass 1 timed out:\n" << pass1_res.combined_output;
    ASSERT_EQ(pass1_res.exit_code, kProcessSuccessExitCode)
        << "02_translate.exe Pass 1 failed:\n" << pass1_res.combined_output;
    EXPECT_LT(pass1_res.elapsed_seconds, kMaxLiveBatchLatencySeconds)
        << "Pass 1 exceeded 600s read timeout ceiling";
    EXPECT_EQ(pass1_res.combined_output.find("[ERROR]"), std::string::npos);
    EXPECT_NE(pass1_res.combined_output.find("[SPEAKER-CHECK] PASS"), std::string::npos);
    EXPECT_NE(pass1_res.combined_output.find("Batches to run: 1"), std::string::npos);
    const std::string expected_applied_marker =
        "Applied " + std::to_string(kSandboxScriptLineCount) + "/" +
        std::to_string(kSandboxScriptLineCount) + " lines";
    EXPECT_NE(pass1_res.combined_output.find(expected_applied_marker), std::string::npos);

    const std::int64_t pass1_thoughts_tokens =
        ExtractThoughtsTokensFromTelemetryLog(pass1_res.combined_output);
    ASSERT_GT(pass1_thoughts_tokens, 0)
        << "Expected gemini-4-carbon thinkingLevel=MEDIUM to log thoughts=<N> > 0, output:\n"
        << pass1_res.combined_output;
    EXPECT_GE(pass1_thoughts_tokens, kMinCalibratedThoughtTokensFloor)
        << "Expected calibrated reasoning depth >= " << kMinCalibratedThoughtTokensFloor
        << " thought tokens on 40-line Oretsuba batch, got " << pass1_thoughts_tokens;

    const fs::path sandbox_cache_path = sandbox_script_out / "translation_cache_gemini.json";
    const fs::path sandbox_translated_path = sandbox_script_out / "translated_text.json";
    const fs::path sandbox_last_batch_path =
        sandbox_script_out / "last_gemini_translate" / "last_translation.json";
    ASSERT_TRUE(fs::is_regular_file(sandbox_cache_path));
    ASSERT_TRUE(fs::is_regular_file(sandbox_translated_path));
    ASSERT_TRUE(fs::is_regular_file(sandbox_last_batch_path));

    const TranslationCache pass1_cache = LoadCache(sandbox_cache_path.u8string());
    for (std::size_t i = 0; i < kSandboxScriptLineCount; ++i) {
        const std::string* en_ptr = pass1_cache.Find(kSandboxEntries[i].text);
        ASSERT_NE(en_ptr, nullptr) << "Missing cache entry #" << i;
        const std::string& en = *en_ptr;
        EXPECT_FALSE(en.empty());
        EXPECT_FALSE(NeedsTranslation(en)) << "Residual kana/kanji in: " << en;
        EXPECT_FALSE(oretsuba::HasSobJapanese(en)) << "Residual fullwidth/CJK in: " << en;
        EXPECT_TRUE(IsCp932LosslessRoundTrip(en)) << "Non-CP932 character in: " << en;
        EXPECT_FALSE(ContainsAnySubstring(
            en,
            {"[Takashi]", "[Kobato]", "[Asuka]", "[Holy Knight Hawk]",
             "[Shuusuke]", "[Hiyoko]", "[Hayato]", "[Naru]", "[NARRATION]",
             "[タカシ]", "[鷹志]", "[小鳩]", "[明日香]", "[聖騎士ホーク]", "<\""}))
            << "Leaked speaker tag or ruby wrapper in: " << en;
        std::cout << "  [Oretsuba #" << (i + 1U) << "] " << en << "\n";
    }

    const std::string intro_lower = AsciiLower(*pass1_cache.Find(kJpTakashiIntro));
    EXPECT_NE(intro_lower.find("takashi"), std::string::npos) << intro_lower;
    EXPECT_NE(intro_lower.find("misora"), std::string::npos) << intro_lower;

    const std::string feathers_lower = AsciiLower(*pass1_cache.Find(kJpTakashiFeathers));
    EXPECT_NE(feathers_lower.find("gretagik"), std::string::npos) << feathers_lower;
    EXPECT_NE(feathers_lower.find("hawk"), std::string::npos) << feathers_lower;
    EXPECT_TRUE(ContainsAnySubstring(feathers_lower, {"feather", "wing", "plume"}))
        << feathers_lower;

    const std::string kobato_lower = AsciiLower(*pass1_cache.Find(kJpKobatoRaw));
    EXPECT_NE(kobato_lower.find("yanagihara"), std::string::npos) << kobato_lower;

    const std::string hawk_lower = AsciiLower(*pass1_cache.Find(kJpHawkChivalry));
    EXPECT_NE(hawk_lower.find("pegasus"), std::string::npos) << hawk_lower;
    EXPECT_NE(hawk_lower.find("asuka"), std::string::npos) << hawk_lower;
    EXPECT_NE(hawk_lower.find("gretagik"), std::string::npos) << hawk_lower;

    const bj::value translated_json = oretsuba::JsonParseFile(sandbox_translated_path.u8string());
    const auto& out_strings =
        translated_json.as_object().at("s01_01a.sob").as_object().at("strings").as_array();
    ASSERT_EQ(out_strings.size(), kSandboxScriptLineCount);
    for (std::size_t i = 0; i < kSandboxScriptLineCount; ++i) {
        SCOPED_TRACE(i);
        const auto& row_obj = out_strings[i].as_object();
        if (kSandboxEntries[i].speaker_en[0] != '\0') {
            EXPECT_EQ(row_obj.at("speaker_en").as_string(), kSandboxEntries[i].speaker_en);
        } else {
            EXPECT_EQ(row_obj.if_contains("speaker_en"), nullptr);
        }
    }

    const fs::path sandbox_tsv_path = sandbox_game / "translation_table.tsv";
    const ProcessResult tsv_res = RunChildProcessCapture(
        staged_tsv_exe,
        {L"--dir", sandbox.path().wstring(), L"--out", sandbox_tsv_path.wstring()},
        sandbox.path());
    ASSERT_TRUE(tsv_res.completed) << "03_build_tsv.exe timed out:\n" << tsv_res.combined_output;
    ASSERT_EQ(tsv_res.exit_code, kProcessSuccessExitCode)
        << "03_build_tsv.exe failed:\n" << tsv_res.combined_output;
    ASSERT_TRUE(fs::is_regular_file(sandbox_tsv_path));

    const oretsuba::Bytes tsv_bytes = oretsuba::ReadFileBytes(sandbox_tsv_path.u8string());
    const std::string tsv_text(reinterpret_cast<const char*>(tsv_bytes.data()), tsv_bytes.size());
    std::map<std::string, std::string> tsv_map;
    std::istringstream tsv_stream(tsv_text);
    std::string row;
    std::size_t row_count = 0U;
    while (std::getline(tsv_stream, row)) {
        if (!row.empty() && row.back() == '\r') {
            row.pop_back();
        }
        if (row.empty()) {
            continue;
        }
        ++row_count;
        const std::size_t tab_pos = row.find('\t');
        ASSERT_NE(tab_pos, std::string::npos) << "Malformed TSV row: " << row;
        const std::string jp_col = row.substr(0, tab_pos);
        const std::string en_col = row.substr(tab_pos + 1U);
        EXPECT_TRUE(IsCp932LosslessRoundTrip(jp_col)) << "Non-CP932 TSV key: " << jp_col;
        EXPECT_TRUE(IsCp932LosslessRoundTrip(en_col)) << "Non-CP932 TSV value: " << en_col;
        tsv_map.emplace(jp_col, en_col);
    }
    EXPECT_GT(row_count, kSandboxScriptLineCount);

    const char* const kKeywaitNormalizedKeys[kExpectedKeywaitPauseRows] = {
        kJpTakashiFeathersNormalizedKey,
        kJpKobatoNormalizedKey,
        kJpAsukaPause,
    };
    for (const char* pause_key : kKeywaitNormalizedKeys) {
        const auto it = tsv_map.find(pause_key);
        ASSERT_NE(it, tsv_map.end()) << "Missing TSV key: " << pause_key;
        EXPECT_NE(it->second.find("$&"), std::string::npos)
            << "Expected Navel '$&' inline keywait pause preserved in TSV value for key '"
            << pause_key << "', got: " << it->second;
    }

    const ProcessResult pass2_res = RunChildProcessCapture(
        staged_translate_exe,
        {L"--dir", sandbox.path().wstring(), L"--batch", batch_arg, L"--max-batches", L"1"},
        sandbox.path());
    std::cout << "[02_translate.exe Pass 2 Resume]\n" << pass2_res.combined_output << "\n";
    ASSERT_TRUE(pass2_res.completed) << "02_translate.exe Pass 2 timed out:\n" << pass2_res.combined_output;
    ASSERT_EQ(pass2_res.exit_code, kProcessSuccessExitCode)
        << "02_translate.exe Pass 2 failed:\n" << pass2_res.combined_output;
    EXPECT_NE(pass2_res.combined_output.find("Unique uncached lines: 0 | Batches to run: 0"),
              std::string::npos)
        << "Expected 0 uncached lines and 0 batches on 2nd-pass resume:\n"
        << pass2_res.combined_output;
    EXPECT_EQ(pass2_res.combined_output.find("Tokens: in="), std::string::npos)
        << "Expected 0 live API calls on immediate 2nd-pass resume:\n"
        << pass2_res.combined_output;

    const TranslationCache pass2_cache = LoadCache(sandbox_cache_path.u8string());
    EXPECT_EQ(pass2_cache.values, pass1_cache.values);
}
