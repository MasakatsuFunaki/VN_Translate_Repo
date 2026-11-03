// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

#include "build_tsv/build_tsv.h"

namespace {

namespace fs = std::filesystem;
using namespace oretsuba::build_tsv;

constexpr std::size_t kCustomTestWrapCols = 24U;

struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& name) {
        path = fs::temp_directory_path() / ("oretsuba_ut_tsv_" + name);
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void WriteText(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::vector<std::string> ReadLines(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    return lines;
}

std::unordered_map<std::string, std::string> ParseTsvMap(const fs::path& p) {
    std::unordered_map<std::string, std::string> out;
    for (const auto& line : ReadLines(p)) {
        if (line.empty()) {
            continue;
        }
        const auto tab = line.find('\t');
        if (tab == std::string::npos) {
            continue;
        }
        out.emplace(line.substr(0, tab), line.substr(tab + 1U));
    }
    return out;
}

}  // namespace

TEST(BuildTsvTest, StripsInlineControlWrappersAndNormalizesKeys) {
    EXPECT_EQ(StripInlineControlWrappers("<10>\\　わあっ！"), "　わあっ！");
    EXPECT_EQ(StripInlineControlWrappers("<-6>\\「おはよう」"), "「おはよう」");
    EXPECT_EQ(StripInlineControlWrappers("$&「待て！」"), "「待て！」");
    EXPECT_EQ(StripInlineControlWrappers("<\"柳原|やなぎはら\">の街"), "柳原の街");
    EXPECT_EQ(StripInlineControlWrappers("[やなぎはら|柳原]の街"), "柳原の街");

    EXPECT_EQ(NormalizeTsvKey("<10>\\　わあっ！\r\n"), "わあっ！");
    EXPECT_EQ(NormalizeTsvKey("　\t 「こんにちは」 \n"), "「こんにちは」");
    EXPECT_EQ(NormalizeTsvKey("_bg01_a"), "");
    EXPECT_EQ(NormalizeTsvKey("se/bell01"), "");
}

TEST(BuildTsvTest, EscapesAndSanitizesTsvValues) {
    EXPECT_EQ(EscapeForTsv("a\\b\tc"), "a\\\\b\\tc");
    EXPECT_EQ(SanitizeTsvValue("[Takashi] \t Hello\r\nworld! "), "Hello world!");
    EXPECT_EQ(SanitizeTsvValue("[NARRATION] A quiet winter morning."), "A quiet winter morning.");
}

TEST(BuildTsvTest, WrapsEnglishLinesAt56ColumnsWithBackslashWithoutSplittingWords) {
    const std::string long_line =
        "The winter wind swept down the crowded streets of Yanagihara as the neon signs flickered to life.";
    const std::string wrapped = WrapEnglishLine(long_line, kDefaultWrapColumns);
    ASSERT_NE(wrapped.find('\\'), std::string::npos);

    std::size_t start = 0;
    while (start <= wrapped.size()) {
        const std::size_t bs = wrapped.find('\\', start);
        const std::string segment =
            (bs == std::string::npos)
                ? wrapped.substr(start)
                : wrapped.substr(start, bs - start);
        EXPECT_LE(segment.size(), kDefaultWrapColumns) << "Segment too long: " << segment;
        if (bs == std::string::npos) {
            break;
        }
        start = bs + 1U;
    }

    // Reconstruct words from wrapped string to verify zero mid-word splits
    std::string reconstructed = wrapped;
    std::replace(reconstructed.begin(), reconstructed.end(), '\\', ' ');
    EXPECT_EQ(reconstructed, long_line);
}

TEST(BuildTsvTest, PreservesMidlinePauseTokenDuringWordWrap) {
    const std::string paused_line =
        "Wait right there for a second!$&Do you really think you can escape from Gretagik so easily?";
    const std::string wrapped = WrapEnglishLine(paused_line, kCustomTestWrapCols);
    EXPECT_NE(wrapped.find("$&"), std::string::npos);
    EXPECT_NE(wrapped.find('\\'), std::string::npos);
}

TEST(BuildTsvTest, BuildsTsvWithSpeakersCacheFillAndConflictResolution) {
    TempDir tmp("builder");
    const fs::path tr_path = tmp.path / "translated_text.json";
    const fs::path cache_path = tmp.path / "translation_cache_gemini.json";
    const fs::path tsv_path = tmp.path / "translation_table.tsv";

    WriteText(tr_path, R"({
  "s01_01a.sob": {
    "file": "s01_01a.sob",
    "strings": [
      {
        "type": "dialogue",
        "speaker": "鷹志",
        "text": "<10>\\「おはようございます、小鳩さん」",
        "translated": "\"Good morning, Kobato-san. It is a rather chilly morning in Yanagihara today, isn't it?\""
      },
      {
        "type": "dialogue",
        "speaker": "鷲介",
        "text": "「よう、元気か？」",
        "translated": "「よう、元気か？」"
      },
      {
        "type": "dialogue",
        "speaker": "隼人",
        "text": "「うるせえ」",
        "translated": "\"Shut up.\""
      },
      {
        "type": "dialogue",
        "speaker": "隼人",
        "text": "「うるせえ」",
        "translated": "\"Keep quiet.\""
      },
      {
        "type": "choice",
        "text": "アレキサンドリアへ行く",
        "translated": "Head over to Cafe Alexandria for the evening shift right away"
      }
    ]
  }
})");

    WriteText(cache_path, R"({
  "「よう、元気か？」": "\"Yo, how's it going?\""
})");

    const TsvBuilder builder;
    const TsvBuildSummary summary = builder.Build(
        tr_path.u8string(), cache_path.u8string(), tsv_path.u8string());

    EXPECT_GT(summary.entry_count, 320U);
    EXPECT_GE(summary.speaker_entry_count, 320U);
    EXPECT_EQ(summary.cache_fill_count, 1U);
    EXPECT_EQ(summary.conflict_count, 1U);

    const auto lines = ReadLines(tsv_path);
    ASSERT_FALSE(lines.empty());
    for (const auto& line : lines) {
        const std::size_t first_tab = line.find('\t');
        ASSERT_NE(first_tab, std::string::npos) << "Missing tab in TSV line: " << line;
        EXPECT_EQ(line.find('\t', first_tab + 1U), std::string::npos)
            << "Multiple tabs in TSV line: " << line;
    }

    const auto map = ParseTsvMap(tsv_path);
    EXPECT_EQ(map.at("鷹志"), "Takashi");
    EXPECT_EQ(map.at("聖騎士ホーク"), "Holy Knight Hawk");
    EXPECT_EQ(map.at("鷲介"), "Shuusuke");
    EXPECT_EQ(map.at("隼人"), "Hayato");

    // Leading <10>\ stripped in normalized key, long English dialogue wrapped with '\'
    ASSERT_NE(map.find("「おはようございます、小鳩さん」"), map.end());
    EXPECT_NE(map.at("「おはようございます、小鳩さん」").find('\\'), std::string::npos);

    // Cache-filled entry
    EXPECT_EQ(map.at("「よう、元気か？」"), "\"Yo, how's it going?\"");

    // First-wins on conflict
    EXPECT_EQ(map.at("「うるせえ」"), "\"Shut up.\"");

    // Choice entry is NOT wrapped with '\'
    EXPECT_EQ(map.at("アレキサンドリアへ行く"),
              "Head over to Cafe Alexandria for the evening shift right away");
}
