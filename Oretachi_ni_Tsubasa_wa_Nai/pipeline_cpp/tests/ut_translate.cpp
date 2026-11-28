// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include "apps/paths.h"
#include "build_tsv/build_tsv.h"
#include "common/util.h"
#include "translate/gemini_client.h"
#include "translate/glossary.h"
#include "translate/speaker_gate.h"
#include "translate/translate_core.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace oretsuba::translate;

constexpr unsigned char kMinPrintableAscii = 0x20U;
constexpr unsigned char kMaxPrintableAscii = 0x7EU;
constexpr std::size_t kExistingCacheEntriesForGuard = 42U;
constexpr int kSmallTestBatchSize = 10;
constexpr std::size_t kMaxMockBatchSize = 2U;
constexpr std::size_t kExpectedFallbackCount = 4U;
constexpr int kFirstNumberedLine = 1;
constexpr int kSecondNumberedLine = 2;
constexpr int kThirdNumberedLine = 3;
constexpr std::size_t kExpectedParsedLineCount = 3U;
constexpr std::size_t kInitialCacheSize = 3U;
constexpr std::size_t kRemainingCacheSize = 2U;
constexpr std::uint8_t kOuterFillByte = 0xCCU;
constexpr std::uint8_t kInnerMarkerThirdByte = 0x90U;
constexpr std::uint8_t kInnerMarkerLastByte = 0x5AU;
constexpr std::size_t kInnerThirdByteOffset = 2U;

struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& name) {
        path = fs::temp_directory_path() / fs::u8path("oretsuba_ut_tr_" + name);
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
    std::size_t max_succeeding_batch_size = kMaxMockBatchSize;

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

    const std::string dirty = "“Hello”—world… ‘test’　!";
    const std::string clean = SanitizeAscii(dirty);
    EXPECT_EQ(clean, "\"Hello\"--world... 'test' !");
    for (unsigned char c : clean) {
        EXPECT_GE(c, kMinPrintableAscii);
        EXPECT_LE(c, kMaxPrintableAscii);
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
    ASSERT_EQ(parsed.size(), kExpectedParsedLineCount);
    EXPECT_EQ(parsed.at(kFirstNumberedLine), "\"Good morning, Kobato.\"");
    EXPECT_EQ(parsed.at(kSecondNumberedLine), "The winter wind blew across Yanagihara.");
    EXPECT_EQ(StripSpeakerTags(parsed.at(kThirdNumberedLine)), "\"Hey there, Hiyoko!\"");
}

TEST(TranslateUnitTest, CachePreservesInsertionOrderAndPurgesFailedEntries) {
    TempDir tmp("cache");
    const fs::path cache_file = tmp.path / "translation_cache_gemini.json";

    TranslationCache cache;
    cache.Set("鷹志", "Takashi");
    cache.Set("「未翻訳」", "「未翻訳」");
    cache.Set("「おはよう」", "\"Good morning.\"");

    EXPECT_EQ(cache.Size(), kInitialCacheSize);
    const std::size_t purged = PurgeFailedEntries(cache);
    EXPECT_EQ(purged, 1U);
    EXPECT_EQ(cache.Size(), kRemainingCacheSize);
    EXPECT_FALSE(cache.Contains("「未翻訳」"));

    SaveCache(cache_file.u8string(), cache);
    const TranslationCache reloaded = LoadCache(cache_file.u8string());
    ASSERT_EQ(reloaded.order.size(), kRemainingCacheSize);
    EXPECT_EQ(reloaded.order[0], "鷹志");
    EXPECT_EQ(reloaded.order[1], "「おはよう」");
}

TEST(TranslateUnitTest, RefusesCacheDiscardWithoutExplicitFlag) {
    TranslateOptions opts;
    opts.retranslate = true;
    opts.discard_cache = false;
    std::string reason;
    EXPECT_TRUE(RefuseCacheDiscard(opts, kExistingCacheEntriesForGuard, &reason));
    EXPECT_NE(reason.find("42"), std::string::npos);

    opts.discard_cache = true;
    EXPECT_FALSE(RefuseCacheDiscard(opts, kExistingCacheEntriesForGuard, &reason));
}

TEST(TranslateUnitTest, SpeakerGateDecomposesAndPreservesInlineControls) {
    SpeakerGate gate;
    const auto decomp = gate.DecomposeInlineControls("<10>\\　<\"柳原|やなぎはら\">の街$&寒い");
    EXPECT_EQ(decomp.leading_control_prefix, "<10>\\");
    EXPECT_EQ(decomp.clean_text, "　柳原の街$&寒い");
    EXPECT_TRUE(decomp.has_midline_pause);

    const std::string preserved =
        gate.PreserveInlineControls("「待て！$&行くな！」", "\"Wait right there! Don't go!\"");
    EXPECT_NE(preserved.find("$&"), std::string::npos);
}

TEST(TranslateUnitTest, AdaptiveFallbackSplitsBatchWhenLargeBatchReturnsEmpty) {
    MockTranslationBackend mock;
    mock.fail_large_batches = true;
    mock.max_succeeding_batch_size = kMaxMockBatchSize;

    const std::vector<std::string> lines = {
        "[Takashi] 「一行目」",
        "[Takashi] 「二行目」",
        "[Takashi] 「三行目」",
        "[Takashi] 「四行目」",
    };
    const auto results =
        TranslateBatchWithFallback(mock, lines, {"[Kobato] ctx"}, {"[Kobato] after"});
    ASSERT_EQ(results.size(), kExpectedFallbackCount);
    for (int i = 1; i <= static_cast<int>(kExpectedFallbackCount); ++i) {
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
    const fs::path tone_file = tmp.path / "TONE_OF_NOVEL.md";

    WriteTextFile(tone_file,
                  "# Tone\n\n"
                  "```text\n"
                  "Synthetic tone prompt.\n"
                  "<character_dossier>\n"
                  "鷹志 聖騎士ホーク 鷲介 隼人 ヨージ ガルーダ 翔 小鳩 明日香 日和子 "
                  "鳴 京 亜衣 狩男 英里子 紀奈子 和馬 春恵 蔵人\n"
                  "</character_dossier>\n"
                  "<terminology>\n"
                  "柳原 = Yanagihara\n"
                  "</terminology>\n"
                  "```\n\n"
                  "### Script Speakers\n"
                  "| Japanese | English |\n"
                  "|---|---|\n"
                  "| 鷹志 | Takashi |\n");

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
    opts.batch_size = kSmallTestBatchSize;
    opts.extracted_file = extracted_file.u8string();
    opts.translated_file = translated_file.u8string();
    opts.cache_file = cache_file.u8string();
    opts.tsv_file = tsv_file.u8string();
    opts.last_batch_log_file = last_batch_file.u8string();
    opts.tone_file = tone_file.u8string();

    TranslateCore core(&mock);
    ASSERT_EQ(core.Run(opts), kExitOk);
    ASSERT_EQ(mock.calls.size(), 1U);
    ASSERT_EQ(mock.calls[0].prompt_lines.size(), kRemainingCacheSize);
    EXPECT_EQ(mock.calls[0].prompt_lines[0], "[Takashi] 「ここはグレタガルドなのか？」");
    EXPECT_EQ(mock.calls[0].prompt_lines[1], "[NARRATION] 　冬の冷たい風が吹いていた。");

    EXPECT_TRUE(fs::is_regular_file(cache_file));
    EXPECT_TRUE(fs::is_regular_file(translated_file));
    EXPECT_TRUE(fs::is_regular_file(tsv_file));
    EXPECT_TRUE(fs::is_regular_file(last_batch_file));
}

TEST(TranslateUnitTest, TranslateCoreHandlesJapaneseUtf8DirectoryAndFilePaths) {
    TempDir tmp("テスト_俺翼_かな");
    const fs::path jp_subdir = tmp.path / fs::u8path("柳原_データ");
    const fs::path extracted_file = jp_subdir / fs::u8path("抽出テキスト.json");
    const fs::path translated_file = jp_subdir / fs::u8path("翻訳済みテキスト.json");
    const fs::path cache_file = jp_subdir / fs::u8path("キャッシュ_gemini.json");
    const fs::path tsv_file = jp_subdir / fs::u8path("翻訳テーブル.tsv");
    const fs::path last_batch_file =
        jp_subdir / fs::u8path("直近バッチ") / fs::u8path("最終翻訳.json");
    const fs::path tone_file = jp_subdir / fs::u8path("作品トーン_俺翼.md");

    WriteTextFile(tone_file,
                  "# Tone\n\n"
                  "```text\n"
                  "Synthetic tone prompt.\n"
                  "<character_dossier>\n"
                  "鷹志 聖騎士ホーク 鷲介 隼人 ヨージ ガルーダ 翔 小鳩 明日香 日和子 "
                  "鳴 京 亜衣 狩男 英里子 紀奈子 和馬 春恵 蔵人\n"
                  "</character_dossier>\n"
                  "<terminology>\n"
                  "柳原 = Yanagihara\n"
                  "</terminology>\n"
                  "```\n\n"
                  "### Script Speakers\n"
                  "| Japanese | English |\n"
                  "|---|---|\n"
                  "| 鷹志 | Takashi |\n"
                  "| 小鳩 | Kobato |\n");

    WriteTextFile(extracted_file, R"({
  "s00_00.sob": {
    "file": "s00_00.sob",
    "strings": [
      {
        "type": "dialogue",
        "speaker": "小鳩",
        "text": "「お兄ちゃん、朝だよ！」"
      },
      {
        "type": "dialogue",
        "speaker": "鷹志",
        "text": "「ああ、もうそんな時間か。」"
      }
    ]
  }
})");

    TranslationCache initial_cache;
    initial_cache.Set("「お兄ちゃん、朝だよ！」", "\"Big brother, it's morning!\"");
    SaveCache(cache_file.u8string(), initial_cache);
    ASSERT_TRUE(fs::is_regular_file(cache_file));
    EXPECT_EQ(CacheEntryCount(cache_file.u8string()), 1U);

    MockTranslationBackend mock;
    TranslateOptions opts;
    opts.batch_size = kSmallTestBatchSize;
    opts.project_dir = jp_subdir.u8string();
    opts.extracted_file = extracted_file.u8string();
    opts.translated_file = translated_file.u8string();
    opts.cache_file = cache_file.u8string();
    opts.tsv_file = tsv_file.u8string();
    opts.last_batch_log_file = last_batch_file.u8string();
    opts.tone_file = tone_file.u8string();

    TranslateCore core(&mock);
    ASSERT_EQ(core.Run(opts), kExitOk);
    ASSERT_EQ(mock.calls.size(), 1U);
    ASSERT_EQ(mock.calls[0].prompt_lines.size(), 1U);
    EXPECT_EQ(mock.calls[0].prompt_lines[0], "[Takashi] 「ああ、もうそんな時間か。」");

    EXPECT_TRUE(fs::is_regular_file(cache_file));
    EXPECT_TRUE(fs::is_regular_file(translated_file));
    EXPECT_TRUE(fs::is_regular_file(tsv_file));
    EXPECT_TRUE(fs::is_regular_file(last_batch_file));

    const TranslationCache final_cache = LoadCache(cache_file.u8string());
    const std::string* cached_kobato = final_cache.Find("「お兄ちゃん、朝だよ！」");
    ASSERT_NE(cached_kobato, nullptr);
    EXPECT_EQ(*cached_kobato, "\"Big brother, it's morning!\"");
    EXPECT_NE(final_cache.Find("「ああ、もうそんな時間か。」"), nullptr);
}

TEST(TranslateUnitTest, UnwrapsSoftDenchiOuterPeAndBacksUpOriginal) {
    TempDir tmp("unwrap");
    const fs::path exe_path = tmp.path / "ORE_TUBA.exe";
    const fs::path orig_path = tmp.path / "ORE_TUBA_orig.exe";

    std::vector<std::uint8_t> synthetic_outer(
        oretsuba::apps::kSoftDenchiOuterSize, kOuterFillByte);
    synthetic_outer[0] = 'M';
    synthetic_outer[1] = 'Z';
    synthetic_outer[oretsuba::apps::kInnerPeOffset] = 'M';
    synthetic_outer[oretsuba::apps::kInnerPeOffset + 1U] = 'Z';
    synthetic_outer[oretsuba::apps::kInnerPeOffset + kInnerThirdByteOffset] =
        kInnerMarkerThirdByte;
    synthetic_outer[oretsuba::apps::kInnerPeOffset + oretsuba::apps::kInnerPeSize - 1U] =
        kInnerMarkerLastByte;

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
    EXPECT_EQ(unwrapped[kInnerThirdByteOffset], kInnerMarkerThirdByte);
    EXPECT_EQ(unwrapped.back(), kInnerMarkerLastByte);

    EXPECT_FALSE(oretsuba::apps::UnwrapInnerPeIfNeeded(tmp.path));
}

TEST(TranslateUnitTest, BuildsGenerateContentRequestWithMediumThinkingLevelAndClampedMaxTokens) {
    namespace bj = boost::json;
    constexpr std::size_t kExpectedSafetyCategoryCount = 5U;
    constexpr std::int64_t kSpecMinOutputTokensFloor = vn::llm::kMinOutputTokensFloor;
    constexpr std::int64_t kSpecMaxOutputTokensCeiling = vn::llm::kMaxOutputTokensCeiling;
    constexpr std::int64_t kNegativeMaxTokensSample = -5;

    bj::object default_body;
    default_body["system"] = bj::array{
        bj::object{{"type", "text"}, {"text", "Primary system instruction."}},
        bj::object{{"type", "text"}, {"text", "Secondary glossary section."}},
    };
    default_body["messages"] = bj::array{
        bj::object{{"role", "user"}, {"content", "1. [Takashi] 「おはよう」"}},
    };

    const bj::object req = BuildGenerateContentRequest(default_body);

    const auto* sys_inst = req.if_contains("systemInstruction");
    ASSERT_NE(sys_inst, nullptr);
    const auto& sys_parts = sys_inst->as_object().at("parts").as_array();
    ASSERT_EQ(sys_parts.size(), 1U);
    EXPECT_EQ(sys_parts[0].as_object().at("text").as_string(),
              "Primary system instruction.\nSecondary glossary section.");

    const auto& contents = req.at("contents").as_array();
    ASSERT_EQ(contents.size(), 1U);
    EXPECT_EQ(contents[0].as_object().at("role").as_string(), "user");
    EXPECT_EQ(contents[0].as_object().at("parts").as_array()[0].as_object().at("text").as_string(),
              "1. [Takashi] 「おはよう」");

    const auto& gen_cfg = req.at("generationConfig").as_object();
    EXPECT_EQ(gen_cfg.at("maxOutputTokens").as_int64(), kSpecMaxOutputTokensCeiling);
    const auto& thinking_cfg = gen_cfg.at("thinkingConfig").as_object();
    EXPECT_EQ(thinking_cfg.at("thinkingLevel").as_string(), "MEDIUM");
    EXPECT_EQ(thinking_cfg.if_contains("thinkingBudget"), nullptr);

    const auto& safety = req.at("safetySettings").as_array();
    ASSERT_EQ(safety.size(), kExpectedSafetyCategoryCount);
    for (std::size_t i = 0; i < safety.size(); ++i) {
        EXPECT_EQ(safety[i].as_object().at("threshold").as_string(), "BLOCK_NONE");
    }

    struct MaxTokensBoundaryCase {
        const char* name;
        std::int64_t requested;
        std::int64_t expected;
    };
    constexpr MaxTokensBoundaryCase kCases[] = {
        {"negative_uses_default_ceiling", kNegativeMaxTokensSample, kSpecMaxOutputTokensCeiling},
        {"zero_uses_default_ceiling", 0, kSpecMaxOutputTokensCeiling},
        {"one_clamped_to_floor", 1, kSpecMinOutputTokensFloor},
        {"below_floor_clamped", kSpecMinOutputTokensFloor - 1, kSpecMinOutputTokensFloor},
        {"exact_floor", kSpecMinOutputTokensFloor, kSpecMinOutputTokensFloor},
        {"interior_above_floor", kSpecMinOutputTokensFloor + 1, kSpecMinOutputTokensFloor + 1},
        {"below_ceiling", kSpecMaxOutputTokensCeiling - 1, kSpecMaxOutputTokensCeiling - 1},
        {"exact_ceiling", kSpecMaxOutputTokensCeiling, kSpecMaxOutputTokensCeiling},
        {"above_ceiling_clamped", kSpecMaxOutputTokensCeiling + 1, kSpecMaxOutputTokensCeiling},
    };
    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.name);
        bj::object body;
        body["max_tokens"] = tc.requested;
        body["messages"] = bj::array{
            bj::object{{"role", "user"}, {"content", "test"}},
        };
        const bj::object out_req = BuildGenerateContentRequest(body);
        EXPECT_EQ(out_req.at("generationConfig").as_object().at("maxOutputTokens").as_int64(),
                  tc.expected);
    }
}

TEST(TranslateUnitTest, ParsesGenerateContentResponseFilteringThoughtsAndRecordingUsage) {
    namespace bj = boost::json;
    constexpr std::int64_t kPromptTokens = 512;
    constexpr std::int64_t kCandidatesTokens = 24;
    constexpr std::int64_t kThoughtsTokens = 1480;
    constexpr std::int64_t kCachedTokens = 320;
    constexpr std::int64_t kExpectedOutputTokens = 1504;
    constexpr std::int64_t kExpectedTotalTokens = 2016;

    const bj::value wire_response = bj::parse(R"json({
      "candidates": [
        {
          "content": {
            "role": "model",
            "parts": [
              {
                "thought": true,
                "text": "1. Internal lookahead reasoning: サメ肌 precedes 巨大なサメ, so keep sharkskin."
              },
              {
                "text": "1. \"Good morning.\"\n2. Terrible sharkskin."
              }
            ]
          },
          "finishReason": "STOP"
        }
      ],
      "usageMetadata": {
        "promptTokenCount": 512,
        "candidatesTokenCount": 24,
        "thoughtsTokenCount": 1480,
        "cachedContentTokenCount": 320
      }
    })json");

    GeminiUsage parsed_usage;
    const bj::object normalized = ParseGenerateContentResponse(wire_response, &parsed_usage);

    const auto& content_arr = normalized.at("content").as_array();
    ASSERT_EQ(content_arr.size(), 1U);
    EXPECT_EQ(content_arr[0].as_object().at("text").as_string(),
              "1. \"Good morning.\"\n2. Terrible sharkskin.");
    EXPECT_EQ(normalized.at("stop_reason").as_string(), "end_turn");

    EXPECT_EQ(parsed_usage.input_tokens, kPromptTokens);
    EXPECT_EQ(parsed_usage.candidates_tokens, kCandidatesTokens);
    EXPECT_EQ(parsed_usage.thoughts_tokens, kThoughtsTokens);
    EXPECT_EQ(parsed_usage.output_tokens, kExpectedOutputTokens);
    EXPECT_EQ(parsed_usage.cache_read_input_tokens, kCachedTokens);
    EXPECT_EQ(parsed_usage.total_tokens, kExpectedTotalTokens);

    const auto& usage_obj = normalized.at("usage").as_object();
    EXPECT_EQ(usage_obj.at("input_tokens").as_int64(), kPromptTokens);
    EXPECT_EQ(usage_obj.at("candidates_tokens").as_int64(), kCandidatesTokens);
    EXPECT_EQ(usage_obj.at("thoughts_tokens").as_int64(), kThoughtsTokens);
    EXPECT_EQ(usage_obj.at("thoughtsTokenCount").as_int64(), kThoughtsTokens);
    EXPECT_EQ(usage_obj.at("output_tokens").as_int64(), kExpectedOutputTokens);
    EXPECT_EQ(usage_obj.at("total_tokens").as_int64(), kExpectedTotalTokens);
    EXPECT_EQ(usage_obj.at("cache_read_input_tokens").as_int64(), kCachedTokens);

    const auto& meta_obj = normalized.at("usageMetadata").as_object();
    EXPECT_EQ(meta_obj.at("thoughtsTokenCount").as_int64(), kThoughtsTokens);

    struct StopReasonCase {
        const char* name;
        const char* wire_json;
        const char* expected_stop_reason;
    };
    constexpr StopReasonCase kStopCases[] = {
        {
            "max_tokens_finish_reason",
            R"json({"candidates":[{"content":{"parts":[{"text":"1. Partial"}]},"finishReason":"MAX_TOKENS"}]})json",
            "max_tokens",
        },
        {
            "prohibited_content_maps_to_refusal",
            R"json({"candidates":[{"content":{"parts":[]},"finishReason":"PROHIBITED_CONTENT"}]})json",
            "refusal",
        },
        {
            "prompt_feedback_block_reason_maps_to_refusal",
            R"json({"promptFeedback":{"blockReason":"SAFETY"}})json",
            "refusal",
        },
    };
    for (const auto& sc : kStopCases) {
        SCOPED_TRACE(sc.name);
        GeminiUsage u;
        const bj::object out = ParseGenerateContentResponse(bj::parse(sc.wire_json), &u);
        EXPECT_EQ(out.at("stop_reason").as_string(), sc.expected_stop_reason);
        EXPECT_EQ(u.thoughts_tokens, 0);
    }
}

TEST(TranslateUnitTest, GeminiClientTracksThoughtTelemetryAcrossValidFailingValidCalls) {
    namespace bj = boost::json;
    static constexpr unsigned kHttpOk = static_cast<unsigned>(vn::llm::kHttpStatusOk);
    static constexpr unsigned kHttpTooManyRequests =
        static_cast<unsigned>(vn::llm::kHttpStatusTooManyRequests);
    static constexpr unsigned kHttpServiceUnavailable = 503U;
    static constexpr std::size_t kFirstSuccessCall = 1U;
    static constexpr std::size_t kRateLimitFaultCall = 2U;
    static constexpr std::size_t kServerErrorFaultCall = 3U;
    static constexpr std::int64_t kFirstCallThoughts = 960;
    static constexpr std::int64_t kSecondCallThoughts = 640;
    static constexpr std::int64_t kCombinedThoughts = 1600;
    static constexpr std::int64_t kSingleLineBatchMaxTokensFloor = vn::llm::kMinOutputTokensFloor;

    std::vector<std::string> captured_targets;
    std::vector<bj::value> captured_requests;
    std::size_t call_index = 0U;

    auto loopback_transport = [&](std::string_view target,
                                  std::string_view request_body,
                                  int /*timeout_sec*/) -> HttpResponse {
        captured_targets.emplace_back(target);
        captured_requests.push_back(bj::parse(request_body));
        ++call_index;

        if (call_index == kFirstSuccessCall) {
            return HttpResponse{
                kHttpOk,
                R"json({
                  "candidates": [{
                    "content": {
                      "parts": [
                        {"thought": true, "text": "Checking Oretsuba speaker register."},
                        {"text": "1. \"Good morning, Big Brother!\""}
                      ]
                    },
                    "finishReason": "STOP"
                  }],
                  "usageMetadata": {
                    "promptTokenCount": 300,
                    "candidatesTokenCount": 18,
                    "thoughtsTokenCount": 960,
                    "cachedContentTokenCount": 100,
                    "totalTokenCount": 1278
                  }
                })json",
            };
        }
        if (call_index == kRateLimitFaultCall) {
            return HttpResponse{kHttpTooManyRequests, R"json({"error":{"code":429}})json"};
        }
        if (call_index == kServerErrorFaultCall) {
            return HttpResponse{kHttpServiceUnavailable, R"json({"error":{"code":503}})json"};
        }
        return HttpResponse{
            kHttpOk,
            R"json({
              "candidates": [{
                "content": {
                  "parts": [
                    {"thought": true, "text": "Second batch reasoning."},
                    {"text": "1. \"Welcome to the rift between dimensions.\""}
                  ]
                },
                "finishReason": "STOP"
              }],
              "usageMetadata": {
                "promptTokenCount": 310,
                "candidatesTokenCount": 22,
                "thoughtsTokenCount": 640,
                "cachedContentTokenCount": 120,
                "totalTokenCount": 972
              }
            })json",
        };
    };

    GeminiClient client("synthetic-unit-key", kDefaultModel, {}, loopback_transport);

    const auto first_batch = client.TranslateBatch(
        {"[Kobato] 「お兄ちゃん、おはよう！」"},
        {"[Takashi] 「ああ、もう朝か。」"},
        {"[Kobato] 「早く朝ごはん食べようよ！」"},
        false);
    ASSERT_EQ(first_batch.size(), 1U);
    EXPECT_EQ(first_batch.at(kFirstNumberedLine), "\"Good morning, Big Brother!\"");
    EXPECT_EQ(client.LastUsage().thoughts_tokens, kFirstCallThoughts);
    EXPECT_EQ(client.CumulativeUsage().thoughts_tokens, kFirstCallThoughts);

    ASSERT_EQ(captured_targets.size(), 1U);
    EXPECT_EQ(captured_targets[0],
              "/v1beta/models/gemini-4-carbon:generateContent?key=synthetic-unit-key");
    const auto& wire_gen_cfg =
        captured_requests[0].as_object().at("generationConfig").as_object();
    EXPECT_EQ(wire_gen_cfg.at("maxOutputTokens").as_int64(), kSingleLineBatchMaxTokensFloor);
    EXPECT_EQ(wire_gen_cfg.at("thinkingConfig").as_object().at("thinkingLevel").as_string(),
              "MEDIUM");
    EXPECT_EQ(wire_gen_cfg.at("thinkingConfig").as_object().if_contains("thinkingBudget"),
              nullptr);

    EXPECT_THROW(
        client.TranslateBatch({"[Takashi] テスト"}, {}, {}, false),
        RateLimitError);
    EXPECT_EQ(client.LastUsage().thoughts_tokens, kFirstCallThoughts);
    EXPECT_EQ(client.CumulativeUsage().thoughts_tokens, kFirstCallThoughts);

    EXPECT_THROW(
        client.TranslateBatch({"[Takashi] テスト"}, {}, {}, false),
        ApiError);
    EXPECT_EQ(client.LastUsage().thoughts_tokens, kFirstCallThoughts);
    EXPECT_EQ(client.CumulativeUsage().thoughts_tokens, kFirstCallThoughts);

    const auto second_batch = client.TranslateBatch(
        {"[Takashi] 「次元の狭間へようこそ。」"}, {}, {}, false);
    ASSERT_EQ(second_batch.size(), 1U);
    EXPECT_EQ(second_batch.at(kFirstNumberedLine),
              "\"Welcome to the rift between dimensions.\"");
    EXPECT_EQ(client.LastUsage().thoughts_tokens, kSecondCallThoughts);
    EXPECT_EQ(client.CumulativeUsage().thoughts_tokens, kCombinedThoughts);

    auto zero_thought_transport = [](std::string_view /*target*/,
                                     std::string_view /*request_body*/,
                                     int /*timeout_sec*/) -> HttpResponse {
        return HttpResponse{
            kHttpOk,
            R"json({
              "candidates": [{
                "content": {
                  "parts": [
                    {"text": "1. Line 1\n2. Line 2\n3. Line 3\n4. Line 4"}
                  ]
                },
                "finishReason": "STOP"
              }],
              "usageMetadata": {
                "promptTokenCount": 400,
                "candidatesTokenCount": 32,
                "thoughtsTokenCount": 0
              }
            })json",
        };
    };
    GeminiClient guarded_client("synthetic-unit-key", kDefaultModel, {}, zero_thought_transport);
    EXPECT_THROW(
        guarded_client.TranslateBatch(
            {
                "[Takashi] 「一行目」",
                "[Kobato] 「二行目」",
                "[Asuka] 「三行目」",
                "[Holy Knight Hawk] 「四行目」",
            },
            {},
            {},
            false),
        ZeroThoughtError);
    EXPECT_EQ(guarded_client.LastUsage().thoughts_tokens, 0);
    EXPECT_EQ(guarded_client.CumulativeUsage().output_tokens, 0);
}

TEST(TranslateUnitTest, GeminiClientRejectsDisallowedArgonModelsAtConstruction) {
    static constexpr unsigned kHttpOk = static_cast<unsigned>(vn::llm::kHttpStatusOk);
    std::size_t transport_calls = 0U;
    auto unused_transport = [&](std::string_view /*target*/,
                                std::string_view /*request_body*/,
                                int /*timeout_sec*/) -> HttpResponse {
        ++transport_calls;
        return HttpResponse{kHttpOk, "{}"};
    };

    constexpr const char* kDisallowedModels[] = {
        "gemini-4-argon",
        "GEMINI-4-ARGON",
        "models/gemini-4-argon-preview",
    };
    for (const char* bad_model : kDisallowedModels) {
        SCOPED_TRACE(bad_model);
        try {
            GeminiClient rejected("synthetic-unit-key", bad_model, {}, unused_transport);
            FAIL() << "Expected ApiError when constructing GeminiClient with " << bad_model;
        } catch (const ApiError& err) {
            const std::string msg = err.what();
            EXPECT_NE(msg.find("argon"), std::string::npos) << msg;
        }
        EXPECT_THROW(GeminiClient("synthetic-unit-key", bad_model), ApiError);
    }
    EXPECT_EQ(transport_calls, 0U);
}

TEST(TranslateUnitTest, GeminiClientMessagesTranslatesDisallowedModelErrorsToApiError) {
    namespace bj = boost::json;
    static constexpr unsigned kHttpOk = static_cast<unsigned>(vn::llm::kHttpStatusOk);
    static constexpr std::int64_t kValidReplyThoughts = 310;

    std::size_t transport_calls = 0U;
    auto loopback_transport = [&](std::string_view /*target*/,
                                  std::string_view /*request_body*/,
                                  int /*timeout_sec*/) -> HttpResponse {
        ++transport_calls;
        return HttpResponse{
            kHttpOk,
            R"json({
              "candidates": [{
                "content": {
                  "parts": [
                    {"thought": true, "text": "Audit check."},
                    {"text": "VERDICT: PASS"}
                  ]
                },
                "finishReason": "STOP"
              }],
              "usageMetadata": {
                "promptTokenCount": 120,
                "candidatesTokenCount": 6,
                "thoughtsTokenCount": 310
              }
            })json",
        };
    };

    GeminiClient client("synthetic-unit-key", kDefaultModel, {}, loopback_transport);

    constexpr char kEmbeddedNulModelLiteral[] = "gemini-4-carbon\0injected";
    const std::string embedded_nul_model(
        kEmbeddedNulModelLiteral, sizeof(kEmbeddedNulModelLiteral) - 1U);
    const std::vector<std::string> bad_models = {
        "gemini-4-argon",
        "GEMINI-4-ARGON",
        "models/gemini-4-argon-preview",
        embedded_nul_model,
    };

    for (const std::string& bad_model : bad_models) {
        SCOPED_TRACE(bad_model);
        bj::object bad_req;
        bad_req["model"] = bad_model;
        bad_req["messages"] = bj::array{
            bj::object{{"role", "user"}, {"content", "Audit sample"}},
        };
        EXPECT_THROW((void)client.Messages(bad_req), ApiError);
    }
    EXPECT_EQ(transport_calls, 0U);

    bj::object valid_req;
    valid_req["model"] = kDefaultModel;
    valid_req["messages"] = bj::array{
        bj::object{{"role", "user"}, {"content", "Audit sample"}},
    };
    const bj::value valid_resp = client.Messages(valid_req);
    EXPECT_EQ(transport_calls, 1U);
    ASSERT_TRUE(valid_resp.is_object());
    EXPECT_EQ(valid_resp.as_object().at("stop_reason").as_string(), "end_turn");
    EXPECT_EQ(client.LastUsage().thoughts_tokens, kValidReplyThoughts);

    GeminiClient moved_client(std::move(client));
    EXPECT_EQ(moved_client.Model(), kDefaultModel);
    EXPECT_EQ(moved_client.LastUsage().thoughts_tokens, kValidReplyThoughts);
    EXPECT_EQ(client.Model(), "");
    EXPECT_EQ(client.LastUsage().thoughts_tokens, 0);
    EXPECT_FALSE(client.WasLastBatchZeroThoughtRefused());
}

TEST(TranslateUnitTest, GeminiClientDelegatesZeroThoughtRetriesAndAccumulatesBilledUsage) {
    namespace bj = boost::json;
    static constexpr unsigned kHttpOk = static_cast<unsigned>(vn::llm::kHttpStatusOk);
    static constexpr int kConfiguredZeroThoughtRetries = vn::llm::kDefaultZeroThoughtRetries;
    static constexpr std::size_t kExpectedMaxLiveAttempts = 4U;
    static constexpr std::size_t kExpectedMaxBackoffSleeps = 3U;
    static constexpr std::size_t kBelowInitialGuardLineCount = 3U;
    static constexpr std::size_t kAtInitialGuardLineCount = 4U;
    static constexpr std::size_t kRecoveryAttemptCount = 3U;
    static constexpr std::size_t kRecoverySleepCount = 2U;
    static constexpr std::int64_t kExpectedAccumulatedInputTokens = 640;
    static constexpr std::int64_t kExpectedAccumulatedCandidatesTokens = 52;
    static constexpr std::int64_t kExpectedAccumulatedThoughtsTokens = 410;
    static constexpr std::int64_t kExpectedAccumulatedOutputTokens = 462;
    static constexpr std::int64_t kExpectedAccumulatedCachedTokens = 150;
    static constexpr std::int64_t kExpectedAccumulatedTotalTokens = 1102;

    auto make_lines = [](std::size_t count) {
        std::vector<std::string> out;
        out.reserve(count);
        for (std::size_t i = 0U; i < count; ++i) {
            out.push_back("[Takashi] 「テスト行" + std::to_string(i + 1U) + "」");
        }
        return out;
    };

    // Case 1: Persistent 0-thought top-level batch of 4 lines executes exact 1 + 3 = 4
    // attempts inside vn::llm_client (never 3 x 4 = 12) and throws ZeroThoughtError.
    {
        std::vector<std::string> request_bodies;
        std::size_t sleep_calls = 0U;
        GeminiClientOptions opts;
        opts.api_key = "synthetic-unit-key";
        opts.model = kDefaultModel;
        opts.zero_thought_max_retries = kConfiguredZeroThoughtRetries;
        opts.sleep_fn = [&](std::chrono::milliseconds) { ++sleep_calls; };
        opts.transport_override = [&](std::string_view /*target*/,
                                      std::string_view body,
                                      int /*timeout_sec*/) -> HttpResponse {
            request_bodies.emplace_back(body);
            return HttpResponse{
                kHttpOk,
                R"json({
                  "candidates": [{
                    "content": {"parts": [{"text": "1. A\n2. B\n3. C\n4. D"}]},
                    "finishReason": "STOP"
                  }],
                  "usageMetadata": {
                    "promptTokenCount": 200,
                    "candidatesTokenCount": 16,
                    "thoughtsTokenCount": 0
                  }
                })json",
            };
        };

        GeminiClient client(std::move(opts));
        EXPECT_THROW(
            (void)client.TranslateBatch(make_lines(kAtInitialGuardLineCount), {}, {}, false),
            ZeroThoughtError);
        ASSERT_EQ(request_bodies.size(), kExpectedMaxLiveAttempts);
        EXPECT_EQ(sleep_calls, kExpectedMaxBackoffSleeps);
        EXPECT_EQ(request_bodies[0].find("Reason carefully in your internal thoughts"),
                  std::string::npos);
        for (std::size_t attempt = 1U; attempt < request_bodies.size(); ++attempt) {
            SCOPED_TRACE(attempt);
            EXPECT_NE(request_bodies[attempt].find("Reason carefully in your internal thoughts"),
                      std::string::npos);
        }
        EXPECT_EQ(client.CumulativeUsage().total_tokens, 0);
    }

    // Case 2: Transient 0-thought on attempts 1..2 recovering on attempt 3 accumulates
    // billed token usage across all 3 attempts without dropping earlier attempts.
    {
        std::size_t call_count = 0U;
        std::size_t sleep_calls = 0U;
        GeminiClientOptions opts;
        opts.api_key = "synthetic-unit-key";
        opts.model = kDefaultModel;
        opts.zero_thought_max_retries = kConfiguredZeroThoughtRetries;
        opts.sleep_fn = [&](std::chrono::milliseconds) { ++sleep_calls; };
        opts.transport_override = [&](std::string_view /*target*/,
                                      std::string_view /*body*/,
                                      int /*timeout_sec*/) -> HttpResponse {
            ++call_count;
            if (call_count == 1U) {
                return HttpResponse{
                    kHttpOk,
                    R"json({
                      "candidates": [{
                        "content": {"parts": [{"text": "1. A\n2. B\n3. C\n4. D"}]},
                        "finishReason": "STOP"
                      }],
                      "usageMetadata": {
                        "promptTokenCount": 200,
                        "candidatesTokenCount": 15,
                        "thoughtsTokenCount": 0,
                        "cachedContentTokenCount": 50
                      }
                    })json",
                };
            }
            if (call_count == 2U) {
                return HttpResponse{
                    kHttpOk,
                    R"json({
                      "candidates": [{
                        "content": {"parts": [{"text": "1. A\n2. B\n3. C\n4. D"}]},
                        "finishReason": "STOP"
                      }],
                      "usageMetadata": {
                        "promptTokenCount": 220,
                        "candidatesTokenCount": 17,
                        "thoughtsTokenCount": 0,
                        "cachedContentTokenCount": 50
                      }
                    })json",
                };
            }
            return HttpResponse{
                kHttpOk,
                R"json({
                  "candidates": [{
                    "content": {
                      "parts": [
                        {"thought": true, "text": "Recovered internal reasoning."},
                        {"text": "1. Line one\n2. Line two\n3. Line three\n4. Line four"}
                      ]
                    },
                    "finishReason": "STOP"
                  }],
                  "usageMetadata": {
                    "promptTokenCount": 220,
                    "candidatesTokenCount": 20,
                    "thoughtsTokenCount": 410,
                    "cachedContentTokenCount": 50
                  }
                })json",
            };
        };

        GeminiClient client(std::move(opts));
        const auto res = client.TranslateBatch(make_lines(kAtInitialGuardLineCount), {}, {}, false);
        ASSERT_EQ(res.size(), kAtInitialGuardLineCount);
        EXPECT_EQ(call_count, kRecoveryAttemptCount);
        EXPECT_EQ(sleep_calls, kRecoverySleepCount);
        const GeminiUsage usage = client.LastUsage();
        EXPECT_EQ(usage.input_tokens, kExpectedAccumulatedInputTokens);
        EXPECT_EQ(usage.candidates_tokens, kExpectedAccumulatedCandidatesTokens);
        EXPECT_EQ(usage.thoughts_tokens, kExpectedAccumulatedThoughtsTokens);
        EXPECT_EQ(usage.output_tokens, kExpectedAccumulatedOutputTokens);
        EXPECT_EQ(usage.cache_read_input_tokens, kExpectedAccumulatedCachedTokens);
        EXPECT_EQ(usage.total_tokens, kExpectedAccumulatedTotalTokens);
    }

    // Case 3: Boundary < 4 lines (3 lines) top-level AND >= 4 lines neutral sub-batch
    // disable retry_on_zero_thoughts (exact 1 HTTP call, 0 sleeps).
    {
        std::size_t call_count = 0U;
        std::size_t sleep_calls = 0U;
        GeminiClientOptions opts;
        opts.api_key = "synthetic-unit-key";
        opts.model = kDefaultModel;
        opts.zero_thought_max_retries = kConfiguredZeroThoughtRetries;
        opts.sleep_fn = [&](std::chrono::milliseconds) { ++sleep_calls; };
        opts.transport_override = [&](std::string_view /*target*/,
                                      std::string_view /*body*/,
                                      int /*timeout_sec*/) -> HttpResponse {
            ++call_count;
            if (call_count == 1U) {
                return HttpResponse{
                    kHttpOk,
                    R"json({
                      "candidates": [{
                        "content": {"parts": [{"text": "1. Sub A\n2. Sub B\n3. Sub C"}]},
                        "finishReason": "STOP"
                      }],
                      "usageMetadata": {
                        "promptTokenCount": 150,
                        "candidatesTokenCount": 12,
                        "thoughtsTokenCount": 0
                      }
                    })json",
                };
            }
            return HttpResponse{
                kHttpOk,
                R"json({
                  "candidates": [{
                    "content": {"parts": [{"text": "1. Sub A\n2. Sub B\n3. Sub C\n4. Sub D"}]},
                    "finishReason": "STOP"
                  }],
                  "usageMetadata": {
                    "promptTokenCount": 150,
                    "candidatesTokenCount": 16,
                    "thoughtsTokenCount": 0
                  }
                })json",
            };
        };

        GeminiClient client(std::move(opts));
        const auto tiny_top =
            client.TranslateBatch(make_lines(kBelowInitialGuardLineCount), {}, {}, false);
        EXPECT_EQ(call_count, 1U);
        EXPECT_EQ(sleep_calls, 0U);
        EXPECT_EQ(tiny_top.size(), kBelowInitialGuardLineCount);

        const auto neutral_sub =
            client.TranslateBatch(make_lines(kAtInitialGuardLineCount), {}, {}, true);
        EXPECT_EQ(call_count, 2U);
        EXPECT_EQ(sleep_calls, 0U);
        EXPECT_EQ(neutral_sub.size(), kAtInitialGuardLineCount);
    }
}

TEST(TranslateUnitTest, GeminiClientRefusesStrikeOneCachingAndPreservesBatchZeroGuardOnUnparsedReply) {
    static constexpr unsigned kHttpOk = static_cast<unsigned>(vn::llm::kHttpStatusOk);
    static constexpr std::size_t kInitialGuardLines = 4U;
    static constexpr std::size_t kBelowSubsequentGuardLines = 15U;
    static constexpr std::size_t kAtSubsequentGuardLines = 16U;
    static constexpr std::size_t kNeutralSubSliceLines = 20U;

    auto make_lines = [](std::size_t count) {
        std::vector<std::string> out;
        out.reserve(count);
        for (std::size_t i = 0U; i < count; ++i) {
            out.push_back("[Kobato] 「台詞" + std::to_string(i + 1U) + "」");
        }
        return out;
    };

    auto make_numbered_wire_reply = [](std::size_t count, std::int64_t thoughts) {
        std::string numbered;
        for (std::size_t i = 0U; i < count; ++i) {
            if (i > 0U) {
                numbered += "\\n";
            }
            numbered += std::to_string(i + 1U) + ". Line " + std::to_string(i + 1U);
        }
        return std::string(R"({"candidates":[{"content":{"parts":[{"text":")") +
               numbered + R"("}]},"finishReason":"STOP"}],"usageMetadata":{)"
               R"("promptTokenCount":300,"candidatesTokenCount":60,"thoughtsTokenCount":)" +
               std::to_string(thoughts) + "}}";
    };

    // Part A: Unparseable Batch 1 reply must NOT call record_top_level_batch_completed(),
    // so the immediate context-free retry on Batch 1 still enforces 1-strike abort at 4 lines.
    {
        std::size_t calls = 0U;
        auto unparsed_then_zero_transport = [&](std::string_view /*target*/,
                                                std::string_view /*body*/,
                                                int /*timeout_sec*/) -> HttpResponse {
            ++calls;
            if (calls == 1U) {
                return HttpResponse{
                    kHttpOk,
                    R"json({
                      "candidates": [{
                        "content": {"parts": [{"text": "Unnumbered commentary without indices."}]},
                        "finishReason": "STOP"
                      }],
                      "usageMetadata": {
                        "promptTokenCount": 250,
                        "candidatesTokenCount": 12,
                        "thoughtsTokenCount": 520
                      }
                    })json",
                };
            }
            return HttpResponse{
                kHttpOk,
                make_numbered_wire_reply(kInitialGuardLines, 0),
            };
        };

        GeminiClient client("synthetic-unit-key", kDefaultModel, {}, unparsed_then_zero_transport);
        EXPECT_THROW(
            (void)TranslateBatchWithFallback(
                client,
                make_lines(kInitialGuardLines),
                {"[Takashi] Prior context"},
                {"[Asuka] Lookahead context"}),
            ZeroThoughtError);
        EXPECT_EQ(calls, 2U);
    }

    // Part B: After valid Batch 1, 15-line 0-thought batch is accepted (B-1), 16-line 0-thought
    // batch records Strike 1 and returns empty WITHOUT triggering context-free/neutral/sub-batch
    // retries, 20-line neutral sub-slice is immune, and a second consecutive 16-line 0-thought
    // top-level batch throws ZeroThoughtError.
    {
        std::size_t calls = 0U;
        std::int64_t next_thoughts = 600;
        std::size_t next_reply_lines = kInitialGuardLines;

        auto scripted_transport = [&](std::string_view /*target*/,
                                      std::string_view /*body*/,
                                      int /*timeout_sec*/) -> HttpResponse {
            ++calls;
            return HttpResponse{
                kHttpOk,
                make_numbered_wire_reply(next_reply_lines, next_thoughts),
            };
        };

        GeminiClient client("synthetic-unit-key", kDefaultModel, {}, scripted_transport);

        // Batch 1 (4 lines, thoughts = 600): succeeds and completes top-level Batch 0.
        const auto batch1 = TranslateBatchWithFallback(
            client, make_lines(kInitialGuardLines), {"[Takashi] ctx"}, {"[Kobato] after"});
        ASSERT_EQ(batch1.size(), kInitialGuardLines);
        EXPECT_FALSE(client.WasLastBatchZeroThoughtRefused());
        EXPECT_EQ(calls, 1U);

        // Seam B-1 (15 lines, thoughts = 0): below 16-line threshold -> accepted, 0 strikes.
        next_thoughts = 0;
        next_reply_lines = kBelowSubsequentGuardLines;
        const auto seam15 = TranslateBatchWithFallback(
            client, make_lines(kBelowSubsequentGuardLines), {"[Takashi] ctx"}, {"[Kobato] after"});
        ASSERT_EQ(seam15.size(), kBelowSubsequentGuardLines);
        EXPECT_FALSE(client.WasLastBatchZeroThoughtRefused());
        EXPECT_EQ(calls, 2U);

        // Seam B (16 lines, thoughts = 0) with non-empty context: Strike 1 -> refused ({}),
        // exact 1 HTTP call (no context-free retry that would double-count Strike 1 into 2,
        // and no neutral/sub-batch fallback!).
        next_thoughts = 0;
        next_reply_lines = kAtSubsequentGuardLines;
        const auto strike1 = TranslateBatchWithFallback(
            client, make_lines(kAtSubsequentGuardLines), {"[Takashi] ctx"}, {"[Kobato] after"});
        EXPECT_EQ(strike1.size(), 0U);
        EXPECT_TRUE(client.WasLastBatchZeroThoughtRefused());
        EXPECT_EQ(calls, 3U);

        // Sub-slice immunity: 20-line neutral sub-batch (use_neutral_prompt = true, thoughts = 0)
        // neither throws ZeroThoughtError nor refuses results.
        next_thoughts = 0;
        next_reply_lines = kNeutralSubSliceLines;
        const auto neutral_sub =
            client.TranslateBatch(make_lines(kNeutralSubSliceLines), {}, {}, true);
        ASSERT_EQ(neutral_sub.size(), kNeutralSubSliceLines);
        EXPECT_FALSE(client.WasLastBatchZeroThoughtRefused());
        EXPECT_EQ(calls, 4U);

        // Strike 2: consecutive 16-line top-level batch with thoughts = 0 throws ZeroThoughtError.
        next_thoughts = 0;
        next_reply_lines = kAtSubsequentGuardLines;
        EXPECT_THROW(
            (void)TranslateBatchWithFallback(
                client, make_lines(kAtSubsequentGuardLines), {"[Takashi] ctx"}, {}),
            ZeroThoughtError);
        EXPECT_EQ(calls, 5U);
    }
}

TEST(TranslateUnitTest, StrictCp932ValidationAndLatinFoldingGuardCacheIntegrity) {
    static constexpr std::uint8_t kAsciiUpperA = 0x41U;
    static constexpr std::uint8_t kAsciiDel = 0x7FU;
    static constexpr std::uint8_t kSjisIdeographicSpaceLead = 0x81U;
    static constexpr std::uint8_t kSjisIdeographicSpaceTrail = 0x40U;
    static constexpr std::size_t kExpectedTwoBytes = 2U;
    static constexpr std::size_t kExpectedSurvivingCacheEntries = 2U;

    const auto empty_cp932 = oretsuba::Utf8ToCp932Strict("");
    ASSERT_TRUE(empty_cp932.has_value());
    EXPECT_EQ(empty_cp932->size(), 0U);

    const auto ascii_del_cp932 = oretsuba::Utf8ToCp932Strict("A\x7F");
    ASSERT_TRUE(ascii_del_cp932.has_value());
    ASSERT_EQ(ascii_del_cp932->size(), kExpectedTwoBytes);
    EXPECT_EQ((*ascii_del_cp932)[0], kAsciiUpperA);
    EXPECT_EQ((*ascii_del_cp932)[1], kAsciiDel);

    const auto fullwidth_space_cp932 = oretsuba::Utf8ToCp932Strict("　");
    ASSERT_TRUE(fullwidth_space_cp932.has_value());
    ASSERT_EQ(fullwidth_space_cp932->size(), kExpectedTwoBytes);
    EXPECT_EQ((*fullwidth_space_cp932)[0], kSjisIdeographicSpaceLead);
    EXPECT_EQ((*fullwidth_space_cp932)[1], kSjisIdeographicSpaceTrail);

    struct InvalidCp932Case {
        const char* name;
        std::string_view input;
    };
    constexpr char kEmbeddedNulSample[] = "Hello\0World";
    const std::string embedded_nul(kEmbeddedNulSample, sizeof(kEmbeddedNulSample) - 1U);
    const InvalidCp932Case kInvalidCases[] = {
        {"embedded_nul_byte", embedded_nul},
        {"lone_0x80_continuation_byte", "\x80"},
        {"invalid_0xff_byte", "\xFF"},
        {"truncated_utf8_lead", "\xC3"},
        {"invalid_utf8_continuation_pair", "\xC3("},
        {"unmapped_latin_diacritic_naive", "naïve"},
        {"unmapped_euro_sign", "500€"},
        {"unmapped_four_byte_emoji", "🦅"},
    };
    for (const auto& tc : kInvalidCases) {
        SCOPED_TRACE(tc.name);
        EXPECT_FALSE(oretsuba::Utf8ToCp932Strict(tc.input).has_value());
    }

    const std::string rich_latin =
        "«Café\u00A0Alexandria»—Harue's crêpe • naïve façade, señor Ōtori & Ægir œuf ß!\u200B";
    const std::string folded = SanitizeAscii(rich_latin);
    EXPECT_EQ(
        folded,
        "\"Cafe Alexandria\"--Harue's crepe * naive facade, senor Otori & AEgir oeuf ss!");
    EXPECT_TRUE(oretsuba::Utf8ToCp932Strict(folded).has_value());

    // Malformed UTF-8 bytes must not be masked into ASCII by SanitizeAscii.
    const std::string malformed_utf8 = "bad\xC3(seq";
    EXPECT_FALSE(oretsuba::Utf8ToCp932Strict(SanitizeAscii(malformed_utf8)).has_value());

    constexpr char kNullByteValueLiteral[] = "Null\0byte";
    TranslationCache cache;
    const std::map<int, std::string> batch_results = {
        {1, "[Harue] «Café\u00A0crêpe • naïve señor!»\u200B"},
        {2, "Price is 500€"},
        {3, "Hawk emblem 🦅"},
        {4, "\"Welcome to グレタガルド!\""},
        {5, "bad\xFFutf8"},
        {6, std::string(kNullByteValueLiteral, sizeof(kNullByteValueLiteral) - 1U)},
    };

    EXPECT_TRUE(ApplyLineResult("「クレープだよ！」", batch_results, 1, cache));
    EXPECT_FALSE(ApplyLineResult("「五百ユーロだ」", batch_results, 2, cache));
    EXPECT_FALSE(ApplyLineResult("「鷹の紋章」", batch_results, 3, cache));
    EXPECT_FALSE(ApplyLineResult("<10>\\「ようこそ！」", batch_results, 4, cache));
    EXPECT_FALSE(ApplyLineResult("「壊れた文字列」", batch_results, 5, cache));
    EXPECT_FALSE(ApplyLineResult("「ヌル文字」", batch_results, 6, cache));

    ASSERT_EQ(cache.Size(), 1U);
    const std::string* harue_en = cache.Find("「クレープだよ！」");
    ASSERT_NE(harue_en, nullptr);
    EXPECT_EQ(*harue_en, "\"Cafe crepe * naive senor!\"");

    // PurgeFailedEntries removes pre-existing non-CP932 and leaked-JP cache entries
    // while keeping valid speaker and dialogue entries intact.
    cache.Set("鷹志", "Takashi");
    cache.Set("「ユーロ」", "Costs 10€");
    cache.Set("「混入」", "\"Wait, お兄ちゃん!\"");
    cache.Set("「空白」", "   ");
    const std::size_t purged = PurgeFailedEntries(cache);
    EXPECT_EQ(purged, 3U);
    EXPECT_EQ(cache.Size(), kExpectedSurvivingCacheEntries);
    EXPECT_NE(cache.Find("鷹志"), nullptr);
    EXPECT_NE(cache.Find("「クレープだよ！」"), nullptr);
}
