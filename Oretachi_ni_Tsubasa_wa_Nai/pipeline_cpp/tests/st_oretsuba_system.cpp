// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

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
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
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
constexpr std::size_t kJudgeMaxOutputTokens = 4096U;

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

TEST(TranslateSystemTest, MovingWindowArgonJudgeAuditAcrossThreeNovelWindows) {
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
    std::cout << "[Argon Judge Report]\n" << judge_text << "\n";
    EXPECT_NE(judge_text.find("VERDICT: PASS"), std::string::npos)
        << "Judge did not return VERDICT: PASS:\n" << judge_text;
}
