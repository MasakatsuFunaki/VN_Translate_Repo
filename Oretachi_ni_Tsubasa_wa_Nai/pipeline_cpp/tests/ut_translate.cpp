// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include "apps/paths.h"
#include "build_tsv/build_tsv.h"
#include "common/util.h"
#include "translate/gemini_client.h"
#include "translate/glossary.h"
#include "translate/speaker_gate.h"
#include "translate/translate_core.h"

namespace {

namespace fs = std::filesystem;
namespace bj = boost::json;
using namespace oretsuba::translate;

constexpr std::size_t kMinExpectedGlossarySpeakers = 323U;
constexpr std::size_t kExpectedCorpusSpeakers = 323U;
constexpr std::size_t kMovingWindowSize = 8U;
constexpr std::size_t kJudgeMaxOutputTokens = 4096U;

struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& name) {
        path = fs::temp_directory_path() / ("oretsuba_ut_tr_" + name);
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void WriteTextFile(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

class MockTranslationBackend final : public ITranslationBackend {
public:
    struct RecordedCall {
        std::vector<std::string> prompt_lines;
        std::vector<std::string> previous_context;
        std::vector<std::string> context_after;
        bool use_neutral_prompt = false;
    };

    std::vector<RecordedCall> calls;
    bool fail_large_batches = false;
    std::size_t max_succeeding_batch_size = 2U;

    std::map<int, std::string> TranslateBatch(
        const std::vector<std::string>& prompt_lines,
        const std::vector<std::string>& previous_context,
        const std::vector<std::string>& context_after,
        bool use_neutral_prompt) override {
        calls.push_back(RecordedCall{
            prompt_lines, previous_context, context_after, use_neutral_prompt});

        if (fail_large_batches && prompt_lines.size() > max_succeeding_batch_size) {
            return {};
        }

        std::map<int, std::string> out;
        for (std::size_t i = 0; i < prompt_lines.size(); ++i) {
            out[static_cast<int>(i + 1U)] =
                "Translated line #" + std::to_string(i + 1U);
        }
        return out;
    }
};

}  // namespace

TEST(TranslateUnitTest, NeedsTranslationDistinguishesJapaneseFromAsciiAndDigits) {
    EXPECT_TRUE(NeedsTranslation("「おはようございます」"));
    EXPECT_TRUE(NeedsTranslation("　冬の柳原は冷たい風が吹いていた。"));
    EXPECT_TRUE(NeedsTranslation("ｱｲｳｴｵ"));
    EXPECT_FALSE(NeedsTranslation(""));
    EXPECT_FALSE(NeedsTranslation("   \t\r\n　"));
    EXPECT_FALSE(NeedsTranslation("12345"));
    EXPECT_FALSE(NeedsTranslation("１２３４５"));
    EXPECT_FALSE(NeedsTranslation("BGM_01"));
}

TEST(TranslateUnitTest, StripsSpeakerTagsAndSanitizesFullwidthPunctuationToAscii) {
    EXPECT_EQ(StripSpeakerTags("[Takashi] \"Good morning.\""), "\"Good morning.\"");
    EXPECT_EQ(StripSpeakerTags("[Holy Knight Hawk] Hark!"), "Hark!");
    EXPECT_EQ(StripSpeakerTags("[NARRATION] The snow fell."), "The snow fell.");

    const std::string dirty = "“Hello”—world… ‘test’\xE3\x80\x80!";
    const std::string clean = SanitizeAscii(dirty);
    EXPECT_EQ(clean, "\"Hello\"--world... 'test' !");
    for (unsigned char c : clean) {
        EXPECT_GE(c, 0x20U);
        EXPECT_LE(c, 0x7EU);
    }
}

TEST(TranslateUnitTest, BuildsUserPromptWith80LineContextAnd20LineLookahead) {
    const std::vector<std::string> prev = {"[Takashi] Previous line 1", "[Kobato] Previous line 2"};
    const std::vector<std::string> batch = {"[Takashi] 「おはよう」", "[NARRATION] 　風が吹いた。"};
    const std::vector<std::string> after = {"[Shuusuke] 「よお」"};

    const std::string prompt = BuildUserPrompt(batch, prev, after);
    EXPECT_NE(prompt.find("<previous_context>"), std::string::npos);
    EXPECT_NE(prompt.find("[Takashi] Previous line 1"), std::string::npos);
    EXPECT_NE(prompt.find("<lines_to_translate>"), std::string::npos);
    EXPECT_NE(prompt.find("1. [Takashi] 「おはよう」"), std::string::npos);
    EXPECT_NE(prompt.find("2. [NARRATION] 　風が吹いた。"), std::string::npos);
    EXPECT_NE(prompt.find("<context_after>"), std::string::npos);
    EXPECT_NE(prompt.find("[Shuusuke] 「よお」"), std::string::npos);
}

TEST(TranslateUnitTest, ParsesNumberedResponsesRobustly) {
    const std::string raw = R"(1. "Good morning, Kobato."
2: The winter wind blew across Yanagihara.
3) [Shuusuke] "Hey there, Hiyoko!")";
    const auto parsed = ParseNumberedResponse(raw);
    ASSERT_EQ(parsed.size(), 3U);
    EXPECT_EQ(parsed.at(1), "\"Good morning, Kobato.\"");
    EXPECT_EQ(parsed.at(2), "The winter wind blew across Yanagihara.");
    EXPECT_EQ(StripSpeakerTags(parsed.at(3)), "\"Hey there, Hiyoko!\"");
}

TEST(TranslateUnitTest, CachePreservesInsertionOrderAndPurgesFailedEntries) {
    TempDir tmp("cache");
    const fs::path cache_file = tmp.path / "translation_cache_gemini.json";

    TranslationCache cache;
    cache.Set("鷹志", "Takashi");
    cache.Set("「未翻訳」", "「未翻訳」");  // Failed entry (value == key with JP)
    cache.Set("「おはよう」", "\"Good morning.\"");

    EXPECT_EQ(cache.Size(), 3U);
    const std::size_t purged = PurgeFailedEntries(cache);
    EXPECT_EQ(purged, 1U);
    EXPECT_EQ(cache.Size(), 2U);
    EXPECT_FALSE(cache.Contains("「未翻訳」"));

    SaveCache(cache_file.u8string(), cache);
    const TranslationCache reloaded = LoadCache(cache_file.u8string());
    ASSERT_EQ(reloaded.order.size(), 2U);
    EXPECT_EQ(reloaded.order[0], "鷹志");
    EXPECT_EQ(reloaded.order[1], "「おはよう」");
}

TEST(TranslateUnitTest, RefusesCacheDiscardWithoutExplicitFlag) {
    TranslateOptions opts;
    opts.retranslate = true;
    opts.discard_cache = false;
    std::string reason;
    EXPECT_TRUE(RefuseCacheDiscard(opts, 42U, &reason));
    EXPECT_NE(reason.find("42"), std::string::npos);

    opts.discard_cache = true;
    EXPECT_FALSE(RefuseCacheDiscard(opts, 42U, &reason));
}

TEST(TranslateUnitTest, GlossaryAndSpeakerGatePassOnRealExtractedCorpus) {
    const fs::path proj_dir = fs::u8path(ORETSUBA_PROJECT_DIR);
    const fs::path tone_path = proj_dir / "TONE_OF_NOVEL.md";
    const fs::path extracted_path = proj_dir / "script_output" / "extracted_text.json";

    Glossary glossary;
    ASSERT_TRUE(glossary.LoadFromToneFile(tone_path.u8string()));
    EXPECT_GE(glossary.SpeakerTranslationMap().size(), kMinExpectedGlossarySpeakers);
    EXPECT_EQ(glossary.LookupSpeaker("鷹志").value_or(""), "Takashi");
    EXPECT_EQ(glossary.LookupSpeaker("聖騎士ホーク").value_or(""), "Holy Knight Hawk");
    EXPECT_EQ(glossary.LookupSpeaker("鷲介").value_or(""), "Shuusuke");
    EXPECT_EQ(glossary.LookupSpeaker("隼人").value_or(""), "Hayato");
    EXPECT_EQ(glossary.LookupSpeaker("ヨージ").value_or(""), "Yoji");

    SpeakerGate gate;
    const auto decomp = gate.DecomposeInlineControls("<10>\\　<\"柳原|やなぎはら\">の街$&寒い");
    EXPECT_EQ(decomp.leading_control_prefix, "<10>\\");
    EXPECT_EQ(decomp.clean_text, "　柳原の街$&寒い");
    EXPECT_TRUE(decomp.has_midline_pause);

    const std::string preserved =
        gate.PreserveInlineControls("「待て！$&行くな！」", "\"Wait right there! Don't go!\"");
    EXPECT_NE(preserved.find("$&"), std::string::npos);

    if (fs::is_regular_file(extracted_path)) {
        const SpeakerCheckReport report =
            gate.ValidateExtractedFile(extracted_path.u8string(), glossary);
        EXPECT_TRUE(report.passed) << report.reason;
        EXPECT_EQ(report.corpus_speakers, kExpectedCorpusSpeakers);
    }
}

TEST(TranslateUnitTest,AdaptiveFallbackSplitsBatchWhenLargeBatchReturnsEmpty) {
    MockTranslationBackend mock;
    mock.fail_large_batches = true;
    mock.max_succeeding_batch_size = 2U;

    const std::vector<std::string> lines = {
        "[Takashi] 「一行目」",
        "[Takashi] 「二行目」",
        "[Takashi] 「三行目」",
        "[Takashi] 「四行目」",
    };
    const auto results = TranslateBatchWithFallback(mock, lines, {"[Kobato] ctx"}, {"[Kobato] after"});
    ASSERT_EQ(results.size(), 4U);
    for (int i = 1; i <= 4; ++i) {
        EXPECT_FALSE(results.at(i).empty());
    }
}

TEST(TranslateUnitTest, TranslateCoreRunsWithMockBackendAndGeneratesTsv) {
    TempDir tmp("core");
    const fs::path extracted_file = tmp.path / "extracted_text.json";
    const fs::path translated_file = tmp.path / "translated_text.json";
    const fs::path cache_file = tmp.path / "translation_cache_gemini.json";
    const fs::path tsv_file = tmp.path / "translation_table.tsv";
    const fs::path last_batch_file = tmp.path / "last_gemini_translate" / "last_translation.json";

    WriteTextFile(extracted_file, R"({
  "s00_00.sob": {
    "file": "s00_00.sob",
    "strings": [
      {
        "type": "dialogue",
        "speaker": "鷹志",
        "text": "<10>\\「ここはグレタガルドなのか？」"
      },
      {
        "type": "narration",
        "text": "　冬の冷たい風が吹いていた。"
      }
    ]
  }
})");

    MockTranslationBackend mock;
    TranslateOptions opts;
    opts.batch_size = 10;
    opts.extracted_file = extracted_file.u8string();
    opts.translated_file = translated_file.u8string();
    opts.cache_file = cache_file.u8string();
    opts.tsv_file = tsv_file.u8string();
    opts.last_batch_log_file = last_batch_file.u8string();
    opts.tone_file = (fs::u8path(ORETSUBA_PROJECT_DIR) / "TONE_OF_NOVEL.md").u8string();

    TranslateCore core(&mock);
    ASSERT_EQ(core.Run(opts), kExitOk);
    ASSERT_EQ(mock.calls.size(), 1U);
    ASSERT_EQ(mock.calls[0].prompt_lines.size(), 2U);
    EXPECT_EQ(mock.calls[0].prompt_lines[0], "[Takashi] 「ここはグレタガルドなのか？」");
    EXPECT_EQ(mock.calls[0].prompt_lines[1], "[NARRATION] 　冬の冷たい風が吹いていた。");

    EXPECT_TRUE(fs::is_regular_file(cache_file));
    EXPECT_TRUE(fs::is_regular_file(translated_file));
    EXPECT_TRUE(fs::is_regular_file(tsv_file));
    EXPECT_TRUE(fs::is_regular_file(last_batch_file));
}

TEST(TranslateUnitTest, UnwrapsSoftDenchiOuterPeAndBacksUpOriginal) {
    TempDir tmp("unwrap");
    const fs::path exe_path = tmp.path / "ORE_TUBA.exe";
    const fs::path orig_path = tmp.path / "ORE_TUBA_orig.exe";

    std::vector<std::uint8_t> synthetic_outer(oretsuba::apps::kSoftDenchiOuterSize, 0xCC);
    synthetic_outer[0] = 'M';
    synthetic_outer[1] = 'Z';
    synthetic_outer[oretsuba::apps::kInnerPeOffset] = 'M';
    synthetic_outer[oretsuba::apps::kInnerPeOffset + 1U] = 'Z';
    synthetic_outer[oretsuba::apps::kInnerPeOffset + 2U] = 0x90U;
    synthetic_outer[oretsuba::apps::kInnerPeOffset + oretsuba::apps::kInnerPeSize - 1U] = 0x5AU;

    oretsuba::WriteFileBytes(exe_path.u8string(), synthetic_outer);

    EXPECT_TRUE(oretsuba::apps::UnwrapInnerPeIfNeeded(tmp.path));
    ASSERT_TRUE(fs::is_regular_file(orig_path));
    EXPECT_EQ(fs::file_size(orig_path), oretsuba::apps::kSoftDenchiOuterSize);
    ASSERT_TRUE(fs::is_regular_file(exe_path));
    EXPECT_EQ(fs::file_size(exe_path), oretsuba::apps::kInnerPeSize);

    const auto unwrapped = oretsuba::ReadFileBytes(exe_path.u8string());
    ASSERT_EQ(unwrapped.size(), oretsuba::apps::kInnerPeSize);
    EXPECT_EQ(unwrapped[0], 'M');
    EXPECT_EQ(unwrapped[1], 'Z');
    EXPECT_EQ(unwrapped[2], 0x90U);
    EXPECT_EQ(unwrapped.back(), 0x5AU);

    // Second invocation is an idempotent no-op
    EXPECT_FALSE(oretsuba::apps::UnwrapInnerPeIfNeeded(tmp.path));
}

TEST(TranslateSystemTest, MovingWindowArgonJudgeAuditAcrossThreeNovelWindows) {
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
    ASSERT_GT(all_lines.size(), 1000U);

    const std::vector<std::pair<std::string, std::size_t>> anchors = {
        {"early_novel", 100U},
        {"mid_novel", all_lines.size() / 2U},
        {"late_novel", all_lines.size() - 200U},
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
            const std::string clean_jp = gate.DecomposeInlineControls(all_lines[i].second).clean_text;
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

        judge_payload << "=== Window: " << label << " ===\n";
        for (std::size_t i = 0; i < kMovingWindowSize; ++i) {
            const std::string en = SanitizeAscii(StripSpeakerTags(results.at(static_cast<int>(i + 1U))));
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
            if (block.is_object()) {
                if (const auto* txt = block.as_object().if_contains("text");
                    txt && txt->is_string()) {
                    judge_text += std::string(txt->as_string());
                }
            }
        }
    }
    std::cout << "[Argon Judge Report]\n" << judge_text << "\n";
    EXPECT_NE(judge_text.find("VERDICT: PASS"), std::string::npos)
        << "Judge did not return VERDICT: PASS:\n" << judge_text;
}
