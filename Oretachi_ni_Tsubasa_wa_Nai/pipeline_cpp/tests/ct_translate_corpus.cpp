// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include "translate/glossary.h"
#include "translate/speaker_gate.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace bj = boost::json;

constexpr std::size_t kMinExpectedGlossarySpeakers = 323U;
constexpr std::size_t kExpectedCoreCastSpeakers = 19U;
constexpr std::size_t kExpectedInlineTaggedLines = 2U;

bj::value BuildCoreCastCorpusJson() {
    const std::vector<std::string> core_cast = {
        "鷹志",
        "聖騎士ホーク",
        "鷲介",
        "隼人",
        "ヨージ",
        "ガルーダ",
        "翔",
        "小鳩",
        "明日香",
        "日和子",
        "鳴",
        "京",
        "亜衣",
        "狩男",
        "英里子",
        "紀奈子",
        "和馬",
        "春恵",
        "蔵人",
    };

    bj::array s00_strings;
    for (std::size_t i = 0; i < core_cast.size(); ++i) {
        bj::object entry;
        entry["speaker"] = core_cast[i];
        if (i == 0U) {
            entry["text"] = "<10>\\　<\"柳原|やなぎはら\">の街$&寒い";
        } else if (i == 1U) {
            entry["text"] = "「待て！\\行くな！」";
        } else {
            entry["text"] = "「冬の柳原で会おう」";
        }
        s00_strings.push_back(std::move(entry));
    }

    bj::object s00_obj;
    s00_obj["strings"] = std::move(s00_strings);

    bj::object root;
    root["s00_00.sob"] = std::move(s00_obj);
    return root;
}

}  // namespace

TEST(TranslateCorpusTest, GlossaryAndSpeakerGateValidateTrackedToneAndCoreCastCorpus) {
    using namespace oretsuba::translate;

    const fs::path proj_dir = fs::u8path(ORETSUBA_PROJECT_DIR);
    const fs::path tone_path = proj_dir / "TONE_OF_NOVEL.md";
    ASSERT_TRUE(fs::is_regular_file(tone_path)) << tone_path.u8string();

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

    const bj::value corpus = BuildCoreCastCorpusJson();
    const SpeakerCheckReport report = gate.ValidateCorpus(corpus, glossary);
    EXPECT_TRUE(report.passed) << report.reason;
    EXPECT_EQ(report.corpus_speakers, kExpectedCoreCastSpeakers);
    EXPECT_EQ(report.inline_tag_lines, kExpectedInlineTaggedLines);

    bj::object unmapped_entry;
    unmapped_entry["speaker"] = "未登録スピーカー";
    unmapped_entry["text"] = "「テスト」";
    bj::object unmapped_script;
    unmapped_script["strings"] = bj::array{std::move(unmapped_entry)};
    bj::object unmapped_root;
    unmapped_root["s99_99.sob"] = std::move(unmapped_script);

    const SpeakerCheckReport failed_report = gate.ValidateCorpus(unmapped_root, glossary);
    EXPECT_FALSE(failed_report.passed);
    EXPECT_EQ(failed_report.failed_check, 1);
}
