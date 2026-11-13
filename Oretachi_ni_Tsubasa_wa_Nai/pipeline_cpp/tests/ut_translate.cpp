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
