// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "checker/prompt_codec.h"

#include "common/utf8_text.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace spot_checker {
namespace {

constexpr double kFloatTol = 1e-9;

ScriptEntry make_entry(std::size_t index,
                       std::string_view speaker,
                       std::string_view jp_text,
                       std::string_view current_en) {
    ScriptEntry entry;
    entry.global_index = index;
    entry.script_name = "sc01";
    entry.array_key = "lines";
    entry.entry_index = index;
    entry.type = "TEXT";
    entry.text_key = "text";
    entry.speaker = std::string(speaker);
    entry.jp_text = std::string(jp_text);
    entry.current_en = std::string(current_en);
    return entry;
}

WindowSlice make_passage_slice(std::size_t start, std::size_t count) {
    WindowSlice slice;
    slice.target_start = start;
    for (std::size_t i = 0; i < count; ++i) {
        slice.target.push_back(make_entry(
            start + i, "Kenji", "「今日もいい天気だな。」", "\"Nice weather again today.\""));
        slice.editable_ids.insert(static_cast<int>(i + 1U));
    }
    return slice;
}

TEST(PromptCodecTest, RedactsHLinesAcrossSliceAndFormatsPromptsCleanly) {
    WindowSlice slice;
    slice.target_start = 10U;
    slice.context_before = {
        make_entry(8U, "Kenji", "「廊下を歩こう。」", "\"Let's walk down the hall.\""),
        make_entry(9U, "美咲（Ｈ）", "「熱い吐息が漏れる……」", "\"Hot breath escaped...\""),
    };
    slice.target = {
        make_entry(10U, "Kenji", "「明日の予定は？」", "\"What is tomorrow's plan?\""),
        make_entry(11U, "Yui", "「ふざけないでよ、図書館へ行くわ。」",
                   "\"Quit acting like a cock about the library plan.\""),
        make_entry(12U, "Yui", "「あぁ……んちゅ……」", "\"Touch my pussy right now...\""),
    };
    slice.context_after = {
        make_entry(13U, "美咲（Ｈ）", "「まだ離さないで……」", "\"Don't let go yet...\""),
        make_entry(14U, "Kenji", "「もう夕方だな。」", "\"It is evening already.\""),
    };
    slice.editable_ids = {1, 2, 3};

    // Default empty h_scene_mask leaves every line (including crude banter) unredacted verbatim.
    const WindowSlice unmasked = redact_h_lines_for_prompt(slice);
    ASSERT_EQ(unmasked.context_before.size(), 2U);
    ASSERT_EQ(unmasked.target.size(), 3U);
    ASSERT_EQ(unmasked.context_after.size(), 2U);
    EXPECT_EQ(unmasked.context_before[1].current_en, "\"Hot breath escaped...\"");
    EXPECT_EQ(unmasked.target[1].current_en,
              "\"Quit acting like a cock about the library plan.\"");
    EXPECT_EQ(unmasked.target[2].current_en, "\"Touch my pussy right now...\"");
    EXPECT_EQ(unmasked.context_after[0].current_en, "\"Don't let go yet...\"");
    EXPECT_EQ(unmasked.editable_ids, (std::set<int>{1, 2, 3}));

    // Sustained H-scene mask redacts ONLY masked indices (9, 12, 13) and drops ID 3 from
    // editable_ids, while preserving unmasked dirty banter at index 11 (ID 2) verbatim.
    std::vector<bool> h_scene_mask(20U, false);
    h_scene_mask[9U] = true;
    h_scene_mask[12U] = true;
    h_scene_mask[13U] = true;

    const WindowSlice redacted = redact_h_lines_for_prompt(slice, h_scene_mask);
    ASSERT_EQ(redacted.context_before.size(), 2U);
    ASSERT_EQ(redacted.target.size(), 3U);
    ASSERT_EQ(redacted.context_after.size(), 2U);
    EXPECT_EQ(redacted.editable_ids, (std::set<int>{1, 2}));

    EXPECT_EQ(redacted.context_before[0].current_en, "\"Let's walk down the hall.\"");
    EXPECT_EQ(redacted.target[0].current_en, "\"What is tomorrow's plan?\"");
    EXPECT_EQ(redacted.target[1].current_en,
              "\"Quit acting like a cock about the library plan.\"");
    EXPECT_EQ(redacted.context_after[1].current_en, "\"It is evening already.\"");

    EXPECT_NE(redacted.context_before[1].current_en.find("[REDACTED EXPLICIT SCENE LINE]"),
              std::string::npos);
    EXPECT_NE(redacted.target[2].current_en.find("[REDACTED EXPLICIT SCENE LINE]"),
              std::string::npos);
    EXPECT_NE(redacted.context_after[0].current_en.find("[REDACTED EXPLICIT SCENE LINE]"),
              std::string::npos);

    const std::string user_prompt = build_spot_check_user_prompt(redacted, /*fix_punctuation=*/false);
    EXPECT_NE(user_prompt.find("[EDITABLE SUSPECT]"), std::string::npos);
    EXPECT_NE(user_prompt.find("[READ-ONLY CONTEXT]"), std::string::npos);
    EXPECT_NE(user_prompt.find("[REDACTED EXPLICIT SCENE LINE]"), std::string::npos);
    EXPECT_NE(user_prompt.find("cock about the library plan"), std::string::npos);
    EXPECT_EQ(user_prompt.find("pussy"), std::string::npos);
    EXPECT_EQ(user_prompt.find("熱い吐息"), std::string::npos);

    const ToneGlossary glossary = ToneGlossary::parse_markdown(
        "<formatting_rules>\nInternal batch rule to strip\n</formatting_rules>\n"
        "| 水瀬名雪 | Nayuki |\n");
    const std::string sys_prompt = build_system_prompt(glossary, /*fix_punctuation=*/false);
    const std::string lower_sys_prompt = to_lower_ascii(sys_prompt);
    EXPECT_EQ(sys_prompt.find("Internal batch rule to strip"), std::string::npos);
    EXPECT_NE(sys_prompt.find("Nayuki"), std::string::npos);
    EXPECT_NE(lower_sys_prompt.find("visual novel"), std::string::npos);
    EXPECT_NE(lower_sys_prompt.find("fictional"), std::string::npos);

    const std::vector<WindowSlice> radar_passages = {
        make_passage_slice(0U, 2U),
        make_passage_slice(50U, 2U),
    };
    const std::string radar_prompt = build_radar_pack_prompt(radar_passages, glossary);
    const std::string lower_radar_prompt = to_lower_ascii(radar_prompt);
    EXPECT_NE(radar_prompt.find("Nice weather again today."), std::string::npos);
    EXPECT_NE(lower_radar_prompt.find("visual novel"), std::string::npos);
    EXPECT_NE(lower_radar_prompt.find("fictional"), std::string::npos);
}

TEST(PromptCodecTest, ParsesRadarPackResponseMqmFallbackBoundsDedupAndCorruptJson) {
    const std::vector<WindowSlice> passages = {
        make_passage_slice(0U, 5U),
        make_passage_slice(40U, 6U),
        make_passage_slice(120U, 4U),
    };

    const std::string raw_json = R"([
        "non_object_noise",
        42,
        {"passage_id": 0, "critical_count": 2},
        {"passage_id": 4, "critical_count": 2},
        {"passage_id": 2, "critical_count": 1, "major_count": 2, "minor_count": 3, "suspect_ids": [2, 5]},
        {"passage_id": 2, "mqm_score": 999.0},
        {"passage_id": 3, "mqm_score": -1.0, "critical_count": 0, "major_count": 1, "minor_count": 2, "suspect_ids": [1]},
        {"passage_id": 1, "mqm_score": 14.5, "suspect_ids": [3]}
    ])";

    const Result<std::vector<RadarPassageResult>> parsed =
        parse_radar_pack_response(raw_json, passages);
    ASSERT_TRUE(parsed.is_ok());
    ASSERT_EQ(parsed.value().size(), 3U);

    // Passage 2: fallback 25*1 + 5*2 + 1*3 = 38.0, duplicate passage_id=2 ignored.
    EXPECT_EQ(parsed.value()[0].passage_id, 2);
    EXPECT_EQ(parsed.value()[0].offset, 40U);
    EXPECT_EQ(parsed.value()[0].length, 6U);
    EXPECT_NEAR(parsed.value()[0].mqm_score, 38.0, kFloatTol);
    EXPECT_EQ(parsed.value()[0].suspect_ids, (std::set<int>{2, 5}));

    // Passage 3: negative mqm_score triggers 25*0 + 5*1 + 1*2 = 7.0 fallback.
    EXPECT_EQ(parsed.value()[1].passage_id, 3);
    EXPECT_EQ(parsed.value()[1].offset, 120U);
    EXPECT_EQ(parsed.value()[1].length, 4U);
    EXPECT_NEAR(parsed.value()[1].mqm_score, 7.0, kFloatTol);
    EXPECT_EQ(parsed.value()[1].suspect_ids, (std::set<int>{1}));

    // Passage 1: explicit mqm_score 14.5 preserved.
    EXPECT_EQ(parsed.value()[2].passage_id, 1);
    EXPECT_EQ(parsed.value()[2].offset, 0U);
    EXPECT_EQ(parsed.value()[2].length, 5U);
    EXPECT_NEAR(parsed.value()[2].mqm_score, 14.5, kFloatTol);

    constexpr std::string_view kCorruptInputs[] = {
        "",
        "{unclosed_json",
        R"({"passage_id": 1, "mqm_score": 5.0})",
    };
    for (const std::string_view bad : kCorruptInputs) {
        SCOPED_TRACE(bad);
        const auto err = parse_radar_pack_response(bad, passages);
        EXPECT_FALSE(err.is_ok());
        EXPECT_EQ(err.error_code(), ErrorCode::CorruptJson);
    }
}

TEST(PromptCodecTest, ParsesSpotCheckResponseConfidenceSeamsLocksDedupAndFraming) {
    WindowSlice slice;
    slice.target_start = 20U;
    slice.target = {
        make_entry(20U, "Kenji", "「古い台詞その一。」", "Kenji\r\n\"Old line one.\""),
        make_entry(21U, "Yui", "「古い台詞その二。」", "\n\"Old line two.\"\\@"),
        make_entry(22U, "NARRATION", "古い地の文その三。", "Old narration three."),
        make_entry(23U, "Kenji", "「ロックされた文脈行。」", "\"Locked context line.\""),
    };
    slice.editable_ids = {1, 2, 3};  // ID 4 is read-only context.

    const std::string raw_response = R"([
        {"id": 1, "new_en": "[Kenji] “Fixed—line one!”", "category": "mistranslation",
         "confidence": 0.850, "error_span": "Old", "fix_span": "Fixed", "reason": "Exact seam"},
        {"id": 1, "new_en": "\"Duplicate ID one.\"", "category": "mistranslation",
         "confidence": 0.98, "reason": "Duplicate rejected"},
        {"id": 2, "improved_en": "\"Fixed line two.\"", "category": "character_voice",
         "severity": "critical", "confidence": 1.000, "reason": "Upper confidence seam"},
        {"id": 3, "new_en": "Below confidence seam.", "category": "mistranslation",
         "confidence": 0.849, "reason": "0.849 < 0.850"},
        {"id": 3, "new_en": "Above confidence seam.", "category": "mistranslation",
         "confidence": 1.001, "reason": "1.001 > 1.000"},
        {"id": 3, "new_en": "Missing confidence.", "category": "mistranslation",
         "reason": "Omitted confidence"},
        {"id": 3, "new_en": "String confidence.", "category": "mistranslation",
         "confidence": "0.95", "reason": "Non-numeric confidence"},
        {"id": 4, "new_en": "\"Edited locked line.\"", "category": "mistranslation",
         "confidence": 0.95, "reason": "ID 4 is locked"},
        {"id": 0, "new_en": "Zero ID.", "category": "mistranslation",
         "confidence": 0.95, "reason": "OOB low"},
        {"id": 5, "new_en": "High ID.", "category": "mistranslation",
         "confidence": 0.95, "reason": "OOB high"},
        {"id": 3, "new_en": "Bad category.", "category": "nitpick",
         "confidence": 0.95, "reason": "Unknown category"},
        {"id": 3, "new_en": "Bad severity.", "category": "mistranslation",
         "severity": "blocker", "confidence": 0.95, "reason": "Unknown severity"}
    ])";

    const Result<ParsedSpotCheckBatch> parsed =
        parse_spot_check_response(raw_response, slice, kDefaultMinConfidence);
    ASSERT_TRUE(parsed.is_ok());
    ASSERT_EQ(parsed.value().candidates.size(), 2U);
    EXPECT_EQ(parsed.value().rejected_in_parse, 10U);

    const Improvement& first = parsed.value().candidates[0];
    EXPECT_EQ(first.global_index, 20U);
    EXPECT_EQ(first.improved_en, "Kenji\r\n\"Fixed--line one!\"");
    EXPECT_EQ(first.category, "mistranslation");
    EXPECT_EQ(first.severity, "major");
    EXPECT_NEAR(first.confidence, 0.850, kFloatTol);
    EXPECT_EQ(first.error_span, "Old");
    EXPECT_EQ(first.fix_span, "Fixed");

    const Improvement& second = parsed.value().candidates[1];
    EXPECT_EQ(second.global_index, 21U);
    EXPECT_EQ(second.improved_en, "\n\"Fixed line two.\"\\@");
    EXPECT_EQ(second.category, "character_voice");
    EXPECT_EQ(second.severity, "critical");
    EXPECT_NEAR(second.confidence, 1.0, kFloatTol);

    // Non-finite NaN min_confidence or overflowed +Inf confidence in JSON is rejected:
    const std::string single_valid_json =
        R"([{"id": 3, "new_en": "Clean narration three.", "category": "mistranslation", "confidence": 0.95}])";
    const Result<ParsedSpotCheckBatch> nan_thresh = parse_spot_check_response(
        single_valid_json, slice, std::numeric_limits<double>::quiet_NaN());
    ASSERT_TRUE(nan_thresh.is_ok());
    EXPECT_EQ(nan_thresh.value().candidates.size(), 0U);
    EXPECT_EQ(nan_thresh.value().rejected_in_parse, 1U);

    constexpr std::string_view kCorruptResponses[] = {
        "",
        "[broken_json",
        R"({"id": 1, "new_en": "Object instead of array"})",
    };
    for (const std::string_view bad : kCorruptResponses) {
        SCOPED_TRACE(bad);
        const auto err = parse_spot_check_response(bad, slice, kDefaultMinConfidence);
        EXPECT_FALSE(err.is_ok());
        EXPECT_EQ(err.error_code(), ErrorCode::CorruptJson);
    }
}

}  // namespace
}  // namespace spot_checker
