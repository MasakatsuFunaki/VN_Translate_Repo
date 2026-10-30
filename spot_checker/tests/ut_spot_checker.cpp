// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include "checker/hotspot_detector.h"
#include "checker/spot_checker.h"
#include "client/argon_client.h"
#include "common/util.h"

namespace bj = boost::json;
namespace fs = std::filesystem;
using namespace spot_checker;

namespace {

class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        path_ = fs::temp_directory_path() /
                ("vn_spot_checker_ut_" + std::to_string(++counter));
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_ / "script_output", ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    std::string str() const { return path_.u8string(); }
    fs::path path() const { return path_; }

private:
    fs::path path_;
};

ScriptEntry make_entry(std::size_t idx,
                       const std::string& speaker,
                       const std::string& jp,
                       const std::string& en,
                       const std::string& script = "sn.bin") {
    ScriptEntry e;
    e.global_index = idx;
    e.script_name = script;
    e.array_key = "strings";
    e.entry_index = idx;
    e.text_key = "text";
    e.speaker = speaker;
    e.jp_text = jp;
    e.current_en = en;
    return e;
}

}  // namespace

// ============================================================================
// Unit Tests: Common Utilities & Argon Payload Helpers
// ============================================================================

TEST(SpotCheckerUtil, SanitizeAsciiReplacesSmartPunctuation) {
    const std::string input =
        "Taichi\r\n“Wait—what… isn’t　that–right?”";
    EXPECT_EQ(sanitize_ascii(input),
              "Taichi\r\n\"Wait--what... isn't that--right?\"");
}

TEST(SpotCheckerClient, BuildsArgonPayloadWithThinkingAndJsonMime) {
    const bj::object req = build_argon_request_payload("SYS_PROMPT", "USER_PROMPT");
    ASSERT_TRUE(req.if_contains("systemInstruction"));
    ASSERT_TRUE(req.if_contains("contents"));
    ASSERT_TRUE(req.if_contains("generationConfig"));

    const auto& gen = req.at("generationConfig").as_object();
    EXPECT_EQ(gen.at("responseMimeType").as_string(), "application/json");
    EXPECT_EQ(gen.at("thinkingConfig").as_object().at("thinkingBudget").as_int64(), 2048);

    const bj::value raw_resp = bj::parse(R"({
        "candidates": [{
            "content": {
                "parts": [
                    {"thought": true, "text": "internal reasoning..."},
                    {"text": "[{\"id\": 1, \"new_en\": \"\\\"Fixed.\\\"\"}]"}
                ]
            },
            "finishReason": "STOP"
        }],
        "usageMetadata": {
            "promptTokenCount": 500,
            "candidatesTokenCount": 40,
            "thoughtsTokenCount": 120
        }
    })");
    const ArgonResponse parsed = parse_argon_response_payload(raw_resp);
    EXPECT_EQ(parsed.text, "[{\"id\": 1, \"new_en\": \"\\\"Fixed.\\\"\"}]");
    EXPECT_EQ(parsed.finish_reason, "STOP");
    EXPECT_EQ(parsed.prompt_tokens, 500);
    EXPECT_EQ(parsed.output_tokens, 40);
    EXPECT_EQ(parsed.thought_tokens, 120);
}

// ============================================================================
// Unit Tests: Script Extraction ("strings" and "lines") & Sliding Windows
// ============================================================================

TEST(SpotCheckerExtract, ExtractsFromBothStringsAndLinesSchemas) {
    const bj::value root = bj::parse(R"({
        "sn.bin": {
            "strings": [
                {"type": "dialogue", "speaker": "Taichi", "text": "太一\r\n「おはよう」", "translated": "Taichi\r\n\"Good morning.\""},
                {"type": "other", "text": "bg_01", "translated": "bg_01"},
                {"type": "narration", "speaker": "", "text": "静かな朝だった。", "translated": "It was a quiet morning."}
            ]
        },
        "00_script.SBz": {
            "lines": [
                {"type": "COMMAND", "content": "bgm 01", "translated": "bgm 01"},
                {"type": "NAME", "content": "美希", "translated": "Miki"},
                {"type": "MESSAGE", "content": "「先輩！」", "translated": "\"Senpai!\""}
            ]
        }
    })");

    const auto entries = extract_script_entries(root);
    ASSERT_EQ(entries.size(), 3u);

    EXPECT_EQ(entries[0].script_name, "sn.bin");
    EXPECT_EQ(entries[0].array_key, "strings");
    EXPECT_EQ(entries[0].entry_index, 0u);
    EXPECT_EQ(entries[0].speaker, "Taichi");

    EXPECT_EQ(entries[1].script_name, "sn.bin");
    EXPECT_EQ(entries[1].entry_index, 2u);
    EXPECT_EQ(entries[1].speaker, "NARRATION");

    EXPECT_EQ(entries[2].script_name, "00_script.SBz");
    EXPECT_EQ(entries[2].array_key, "lines");
    EXPECT_EQ(entries[2].entry_index, 2u);
    EXPECT_EQ(entries[2].text_key, "content");
    EXPECT_EQ(entries[2].speaker, "Miki");
}

TEST(SpotCheckerWindow, BuildsSlidingWindowAtStartMiddleAndEnd) {
    std::vector<ScriptEntry> entries;
    for (std::size_t i = 0; i < 10; ++i) {
        entries.push_back(make_entry(
            i, "Taichi", "あ" + std::to_string(i), "Line " + std::to_string(i)));
    }

    // Start of script (offset 0, window 3, before 4, after 2)
    const WindowSlice s0 = build_window_slice(entries, 0, 3, 4, 2);
    EXPECT_TRUE(s0.context_before.empty());
    ASSERT_EQ(s0.target.size(), 3u);
    EXPECT_EQ(s0.target.front().global_index, 0u);
    EXPECT_EQ(s0.target.back().global_index, 2u);
    ASSERT_EQ(s0.context_after.size(), 2u);
    EXPECT_EQ(s0.context_after.front().global_index, 3u);
    EXPECT_EQ(s0.context_after.back().global_index, 4u);

    // Middle of script (offset 4, window 3, before 2, after 2)
    const WindowSlice sm = build_window_slice(entries, 4, 3, 2, 2);
    ASSERT_EQ(sm.context_before.size(), 2u);
    EXPECT_EQ(sm.context_before.front().global_index, 2u);
    EXPECT_EQ(sm.context_before.back().global_index, 3u);
    ASSERT_EQ(sm.target.size(), 3u);
    EXPECT_EQ(sm.target.front().global_index, 4u);
    EXPECT_EQ(sm.target.back().global_index, 6u);
    ASSERT_EQ(sm.context_after.size(), 2u);
    EXPECT_EQ(sm.context_after.front().global_index, 7u);
    EXPECT_EQ(sm.context_after.back().global_index, 8u);

    // End of script (offset 8, window 5, before 3, after 3)
    const WindowSlice se = build_window_slice(entries, 8, 5, 3, 3);
    ASSERT_EQ(se.context_before.size(), 3u);
    EXPECT_EQ(se.context_before.front().global_index, 5u);
    ASSERT_EQ(se.target.size(), 2u);
    EXPECT_EQ(se.target.front().global_index, 8u);
    EXPECT_EQ(se.target.back().global_index, 9u);
    EXPECT_TRUE(se.context_after.empty());
}

TEST(SpotCheckerPrompt, FormatsContextAndTargetAndPreservesSpeakerPrefixOnParse) {
    std::vector<ScriptEntry> entries = {
        make_entry(0, "NARRATION", "朝だ。", "It was morning."),
        make_entry(1, "Taichi",
                   "太一\r\n「腹が減った」",
                   "Taichi\r\n\"As for the stomach, it is hungry.\""),
        make_entry(2, "Misato",
                   "見里\r\n「お弁当ありますよ」",
                   "Misato\r\n\"I have a lunch box.\""),
    };

    WindowSlice slice = build_window_slice(entries, 1, 1, 1, 1);
    const std::string prompt = build_spot_check_user_prompt(slice);
    EXPECT_NE(prompt.find("<context_before>"), std::string::npos);
    EXPECT_NE(prompt.find("<target_window>"), std::string::npos);
    EXPECT_NE(prompt.find("<context_after>"), std::string::npos);
    EXPECT_NE(prompt.find("1. [Taichi]"), std::string::npos);

    // Model returns markdown-fenced JSON, omits "Taichi\r\n" prefix, and uses curly quotes/em-dash
    const std::string fenced_resp =
        "```json\n"
        "[\n"
        "  {\"id\": 1, \"new_en\": \"“I’m starving—really.”\", "
        "\"category\": \"stiff_translationese\", \"confidence\": 0.95, \"reason\": \"Fix stiff literal phrasing\"},\n"
        "  {\"id\": 99, \"new_en\": \"Out of bounds\", \"category\": \"mistranslation\", \"confidence\": 0.99, \"reason\": \"bad id\"}\n"
        "]\n"
        "```";
    const auto improvements = parse_spot_check_response(fenced_resp, slice);
    ASSERT_EQ(improvements.size(), 1u);
    EXPECT_EQ(improvements[0].global_index, 1u);
    EXPECT_EQ(improvements[0].improved_en, "Taichi\r\n\"I'm starving--really.\"");
    EXPECT_EQ(improvements[0].category, "stiff_translationese");
}

// ============================================================================
// System Test 1: Needle in a 5,000-Line Haystack
// ============================================================================

TEST(SpotCheckerSystemTests, SystemTest1_NeedleIn5000LineHaystack) {
    std::vector<ScriptEntry> novel;
    novel.reserve(5000);
    for (std::size_t i = 0; i < 5000; ++i) {
        novel.push_back(make_entry(
            i,
            (i % 2 == 0) ? "Taichi" : "Misato",
            "屋上のアンテナは静かに風を受けていた。",
            "The antenna on the rooftop quietly caught the breeze."));
    }

    // Inject 6 realistic translation defects at lines 2310..2315:
    // 2310: Leftover Japanese brackets & ellipsis
    novel[2310] = make_entry(
        2310, "Touko", "「……」", "「……」");
    // 2311: Inline romaji note "(hippu)"
    novel[2311] = make_entry(
        2311, "Taichi", "「偉大なヒップだ」", "\"What a glorious hip (hippu).\"");
    // 2312 & 2313: Broken 2-line kanji banter callback (脳割れてるんじゃない？ / 割れてる。)
    novel[2312] = make_entry(
        2312, "Touko", "「脳割れてるんじゃない？」", "\"Is something wrong with your head?\"");
    novel[2313] = make_entry(
        2313, "Taichi", "「割れてる。」", "\"Split open.\"");
    // 2314: Dropped cat-pun speech tic かにゃー
    novel[2314] = make_entry(
        2314, "Taichi", "「どうかにゃー」", "\"I wonder about that.\"");
    // 2315: Severe clause truncation + dropped spoken quotes
    novel[2315] = make_entry(
        2315, "Misato",
        "「今日は部活の準備があるから屋上に来てくださいね」",
        "Come.");

    HotspotConfig cfg;
    cfg.window_size = 40;
    cfg.z_threshold = 1.5;
    HotspotDetector detector(cfg);

    std::vector<LineWeaknessScore> scores;
    const auto hotspots = detector.select_hotspots(novel, 5, &scores);
    ASSERT_FALSE(hotspots.empty());

    const HotspotWindow& top = hotspots.front();
    // Verify the #1 ranked hotspot window covers the injected defect cluster [2310..2315]
    EXPECT_LE(top.offset, 2310u);
    EXPECT_GE(top.offset + top.length, 2316u);
    EXPECT_GT(top.z_score, 3.0);
    EXPECT_GE(top.peak_index, 2310u);
    EXPECT_LE(top.peak_index, 2315u);
}

// ============================================================================
// System Test 2: Adversarial "Poisoned LLM" Do-No-Harm Gate
// ============================================================================

TEST(SpotCheckerSystemTests, SystemTest2_AdversarialPoisonedLlmDoNoHarmGate) {
    TempDir tmp;
    const std::string tone_md =
        "# Glossary\n"
        "<name_reference>\n"
        "太一=Taichi, 見里=Misato\n"
        "</name_reference>\n"
        "| `美希` | `Miki` |\n";
    write_file((tmp.path() / "TONE_OF_NOVEL.md").u8string(), tone_md);

    const std::string original_translated_json = json_pretty(bj::parse(R"({
        "sn.bin": {
            "strings": [
                {"type": "dialogue", "speaker": "Taichi", "text": "「今日もいい天気だ」", "translated": "\"Nice weather again today.\""},
                {"type": "dialogue", "speaker": "Taichi", "text": "「見里先輩は屋上にいる（注）」", "translated": "\"Misato-senpai is on the roof (chu).\""},
                {"type": "dialogue", "speaker": "Miki", "text": "「太一先輩、%sをどうぞ\\n美希より」", "translated": "\"Taichi-senpai, as for %s, here you go.\\nFrom Miki.\""},
                {"type": "dialogue", "speaker": "Misato", "text": "「あれは何でしょう」", "translated": "\"As for that, what could it be?\""},
                {"type": "dialogue", "speaker": "Taichi", "text": "「さあな」", "translated": "\"As for that, who knows?\""}
            ]
        }
    })"), 2);
    const std::string translated_path =
        (tmp.path() / "script_output" / "translated_text.json").u8string();
    write_file(translated_path, original_translated_json);

    SpotCheckConfig cfg;
    cfg.game_dir = tmp.str();
    cfg.mode = SpotCheckMode::Linear;
    cfg.offset = 0;
    cfg.window_size = 5;
    cfg.max_windows = 1;

    SpotChecker checker(cfg, [](const std::string&, const std::string&) {
        return R"([
            {"id": 99, "new_en": "\"Out of range ID.\"", "category": "mistranslation", "confidence": 0.99, "reason": "bad id"},
            {"id": 2, "new_en": "\"Senpai is on the roof.\"", "category": "artifact_or_note", "confidence": 0.95, "reason": "Corrupted glossary name by dropping Misato"},
            {"id": 3, "new_en": "\"Taichi-senpai, here you go. From Miki.\"", "category": "stiff_translationese", "confidence": 0.95, "reason": "Dropped %s and \\n placeholders"},
            {"id": 4, "new_en": "What could that be?", "category": "stiff_translationese", "confidence": 0.95, "reason": "Stripped outer double quotes from spoken dialogue"},
            {"id": 5, "new_en": "\"Who knows (shiranai) 「……」\"", "category": "stiff_translationese", "confidence": 0.95, "reason": "Left romaji gloss and CJK brackets"},
            {"id": 5, "new_en": "\"Who knows?\"", "category": "stiff_translationese", "confidence": 0.65, "reason": "Low confidence below 0.85"}
        ])";
    });

    const SpotCheckReport rep = checker.run();
    EXPECT_EQ(rep.improvements_found, 0u);
    EXPECT_GE(rep.candidates_rejected_by_validator, 5u);

    // Also test Trick 1 locking directly when ID 1 is not in editable_ids:
    WindowSlice locked_slice = build_window_slice(checker.entries(), 0, 5, 0, 0);
    locked_slice.editable_ids = {2, 3};  // ID 1 is READ-ONLY CONTEXT
    std::size_t rejected = 0;
    const auto locked_out = parse_spot_check_response(
        R"([{"id": 1, "new_en": "\"Lovely weather again today.\"", "category": "stiff_translationese", "confidence": 0.98, "reason": "Meddled with read-only line 1"}])",
        locked_slice, 0.85, {}, {}, &rejected);
    EXPECT_TRUE(locked_out.empty());
    EXPECT_EQ(rejected, 1u);

    // Verify apply_improvements leaves translated_text.json byte-for-byte unchanged
    const ApplyReport app = checker.apply_improvements();
    EXPECT_EQ(app.improvements_loaded, 0u);
    EXPECT_EQ(read_file_text(translated_path), original_translated_json);
}

// ============================================================================
// System Test 3: Two-Pass Idempotency / Zero-Churn Convergence
// ============================================================================

TEST(SpotCheckerSystemTests, SystemTest3_TwoPassIdempotencyZeroChurnConvergence) {
    TempDir tmp;
    const bj::value initial_json = bj::parse(R"({
        "sn.bin": {
            "strings": [
                {"type": "dialogue", "speaker": "Taichi", "text": "「今日も部活を始めよう」", "translated": "\"Let's start club activities today too.\""},
                {"type": "dialogue", "speaker": "Touko", "text": "「部活の機材を運ぼう」", "translated": "\"Let's carry the 部活 equipment.\""},
                {"type": "dialogue", "speaker": "Taichi", "text": "「彼女が俺を助けてくれたのだ」", "translated": "\"As for that, I helped her out.\""},
                {"type": "dialogue", "speaker": "Misato", "text": "「屋上の風が気持ちいいですね」", "translated": "\"The breeze on the roof feels wonderful, doesn't it?\""}
            ]
        }
    })");
    write_file((tmp.path() / "script_output" / "translated_text.json").u8string(),
               json_pretty(initial_json, 2));

    SpotCheckConfig cfg;
    cfg.game_dir = tmp.str();
    cfg.mode = SpotCheckMode::Hotspot;
    cfg.window_size = 4;
    cfg.max_windows = 1;
    cfg.auto_apply = false;

    // Pass 1: Mock LLM fixes the 2 defective lines (IDs 2 and 3)
    int api_calls_pass1 = 0;
    {
        SpotChecker pass1(cfg, [&](const std::string&, const std::string&) {
            ++api_calls_pass1;
            return R"json([
                {"id": 2, "new_en": "\"Let's carry the club equipment.\"", "category": "artifact_or_note", "confidence": 0.98, "reason": "Translate leaked kanji word"},
                {"id": 3, "new_en": "\"She was the one who saved me.\"", "category": "mistranslation", "confidence": 0.96, "reason": "Fix benefactive reversal and translationese"}
            ])json";
        });
        const SpotCheckReport rep1 = pass1.run();
        EXPECT_EQ(api_calls_pass1, 1);
        EXPECT_EQ(rep1.improvements_found, 2u);

        const ApplyReport app1 = pass1.apply_improvements();
        EXPECT_EQ(app1.script_entries_updated, 2u);
    }

    // Remove the spot check report file to simulate a fresh Pass 2 over the repaired script
    fs::remove(tmp.path() / "script_output" / "argon_spot_check.json");

    // Pass 2: Now all 4 lines have 0 mathematical weakness score -> Suspect-Only Locking
    // unlocks 0 lines and makes 0 API calls (zero churn!)
    int api_calls_pass2 = 0;
    {
        SpotChecker pass2(cfg, [&](const std::string&, const std::string&) {
            ++api_calls_pass2;
            return R"([{"id": 1, "new_en": "\"Let us commence club.\"", "category": "stiff_translationese", "confidence": 0.99, "reason": "unwanted churn"}])";
        });
        const SpotCheckReport rep2 = pass2.run();
        EXPECT_EQ(rep2.editable_lines_unlocked, 0u);
        EXPECT_EQ(api_calls_pass2, 0);
        EXPECT_EQ(rep2.improvements_found, 0u);
    }
}

// ============================================================================
// System Test 4: Adaptive Contagion Boundary Expansion
// ============================================================================

TEST(SpotCheckerSystemTests, SystemTest4_AdaptiveContagionBoundaryExpansion) {
    TempDir tmp;
    bj::array strings_arr;
    for (int i = 0; i < 600; ++i) {
        std::string jp = "「屋上の放送部の準備を進めよう」";
        std::string en = "\"Let's get on with setting up the rooftop broadcasting club.\"";

        // Seed a 6-line defect cluster at 418..423 (so the 40-line hotspot window centered
        // around ~400..440 has defect_rate = 6/40 = 0.15 >= 0.12) plus a defect in the
        // right neighbor window at 450.
        if (i >= 418 && i <= 423) {
            jp = "「彼女が俺を助けてくれたのだ」";
            en = "\"As for that, I helped her out of the 放送部 trouble.\"";
        } else if (i == 450) {
            jp = "「彼女が俺を助けてくれたのだ」";
            en = "\"As for that, I helped her out of the 放送部 trouble.\"";
        }
        strings_arr.push_back(bj::object{
            {"type", "dialogue"},
            {"speaker", "Taichi"},
            {"text", jp},
            {"translated", en},
        });
    }
    bj::object root;
    root["sn.bin"] = bj::object{{"strings", std::move(strings_arr)}};
    write_file((tmp.path() / "script_output" / "translated_text.json").u8string(),
               json_pretty(bj::value(std::move(root)), 2));

    SpotCheckConfig cfg;
    cfg.game_dir = tmp.str();
    cfg.mode = SpotCheckMode::Hotspot;
    cfg.window_size = 40;
    cfg.max_windows = 2;
    cfg.z_threshold = 100.0;  // Only 1 initial hotspot allowed before threshold cutoff,
                              // so the 2nd checked window MUST come from adaptive contagion!

    SpotChecker checker(cfg, [&](const std::string&, const std::string& user_prompt) {
        // Dynamically fix every target line in user_prompt that contains "放送部 trouble"
        bj::array fixes;
        for (int id = 1; id <= 40; ++id) {
            const std::string header = std::to_string(id) + ". [Taichi]";
            const std::size_t pos = user_prompt.find(header);
            if (pos == std::string::npos) continue;
            const std::size_t next_pos =
                user_prompt.find(std::to_string(id + 1) + ". [", pos);
            const std::string block = user_prompt.substr(
                pos, next_pos == std::string::npos ? 240 : next_pos - pos);
            if (block.find("放送部 trouble") != std::string::npos) {
                fixes.push_back(bj::object{
                    {"id", id},
                    {"new_en", "\"She was the one who saved me.\""},
                    {"category", "mistranslation"},
                    {"confidence", 0.97},
                    {"reason", "Fix leaked kanji and benefactive reversal"},
                });
            }
        }
        return bj::serialize(fixes);
    });

    const SpotCheckReport rep = checker.run();
    EXPECT_EQ(rep.hotspots_detected, 1u);
    EXPECT_GE(rep.adaptive_expansions, 1u);
    EXPECT_EQ(rep.windows_checked, 2u);
}

// ============================================================================
// System Test 5: Dual-File Sync, Indent=1+CRLF Format & Cache Key-Set Invariance
// ============================================================================

TEST(SpotCheckerSystemTests, SystemTest5_DualFileSyncAndSchemaPreservation) {
    TempDir tmp;
    const bj::value script_json = bj::parse(R"({
        "sn.bin": {
            "file": "sn.bin",
            "total_entries": 2,
            "strings": [
                {
                    "index": 10,
                    "file": "sn.bin",
                    "offset": 1234,
                    "type": "dialogue",
                    "speaker": "Taichi",
                    "text": "太一\r\n「放送部の準備をしよう」",
                    "translated": "Taichi\r\n\"As for that, let's prepare 放送部.\""
                },
                {
                    "index": 11,
                    "file": "sn.bin",
                    "offset": 1290,
                    "type": "narration",
                    "speaker": "",
                    "text": "風が屋上を吹き抜けた。",
                    "translated": "As for the wind, it blew across the 屋上."
                }
            ]
        }
    })");
    const std::string translated_path =
        (tmp.path() / "script_output" / "translated_text.json").u8string();
    // Write translated_text.json using CROSS_CHANNEL's exact indent = 1 + CRLF format
    write_json_preserving_style(translated_path, script_json, 1, true);

    // Create both translation_cache_anthropic.json and translation_cache_gemini.json
    // with a specific insertion order: Z_key, target_key, A_key.
    // Note: Entry 1 ("風が屋上を吹き抜けた。") is intentionally NOT in the cache to
    // verify that sync_improvements_to_disk NEVER inserts new keys into the cache!
    const std::vector<std::pair<std::string, std::string>> initial_cache = {
        {"ゼット", "Z_value"},
        {"太一\r\n「放送部の準備をしよう」", "Taichi\r\n\"As for that, let's prepare 放送部.\""},
        {"エー", "A_value"},
    };
    const std::string anthropic_cache_path =
        (tmp.path() / "script_output" / "translation_cache_anthropic.json").u8string();
    const std::string gemini_cache_path =
        (tmp.path() / "script_output" / "translation_cache_gemini.json").u8string();
    save_ordered_cache(initial_cache, anthropic_cache_path);
    save_ordered_cache(initial_cache, gemini_cache_path);

    SpotCheckConfig cfg;
    cfg.game_dir = tmp.str();
    cfg.mode = SpotCheckMode::Hotspot;
    cfg.window_size = 10;
    cfg.max_windows = 1;
    cfg.auto_apply = false;

    SpotChecker checker(cfg, [](const std::string&, const std::string&) {
        return R"([
            {"id": 1, "new_en": "\"Let's get the broadcasting club ready.\"", "category": "artifact_or_note", "confidence": 0.99, "reason": "Fix leaked kanji and stiff calque"},
            {"id": 2, "new_en": "The wind swept across the rooftop.", "category": "artifact_or_note", "confidence": 0.99, "reason": "Fix leaked kanji in narration"}
        ])";
    });

    const SpotCheckReport rep = checker.run();
    ASSERT_EQ(rep.improvements_found, 2u);

    const ApplyReport app = checker.apply_improvements();
    EXPECT_EQ(app.improvements_loaded, 2u);
    EXPECT_EQ(app.script_entries_updated, 2u);
    EXPECT_EQ(app.cache_entries_updated, 2u);  // 1 existing key updated in each of the 2 caches

    // Verify indent = 1 + CRLF style was preserved in translated_text.json
    const JsonFormatStyle detected_style = detect_json_format_style(translated_path, 2, false);
    EXPECT_EQ(detected_style.indent, 1);
    EXPECT_TRUE(detected_style.use_crlf);

    // Verify all schema keys in translated_text.json are intact
    const bj::value updated_root = json_parse_file(translated_path);
    const auto& sn_obj = updated_root.as_object().at("sn.bin").as_object();
    EXPECT_EQ(sn_obj.at("file").as_string(), "sn.bin");
    EXPECT_EQ(sn_obj.at("total_entries").as_int64(), 2);
    const auto& row0 = sn_obj.at("strings").as_array()[0].as_object();
    EXPECT_EQ(row0.at("index").as_int64(), 10);
    EXPECT_EQ(row0.at("file").as_string(), "sn.bin");
    EXPECT_EQ(row0.at("offset").as_int64(), 1234);
    EXPECT_EQ(row0.at("type").as_string(), "dialogue");
    EXPECT_EQ(row0.at("speaker").as_string(), "Taichi");
    EXPECT_EQ(row0.at("translated").as_string(),
              "Taichi\r\n\"Let's get the broadcasting club ready.\"");

    // Verify exact insertion order preserved AND no new keys were added to either cache!
    for (const std::string& cpath : {anthropic_cache_path, gemini_cache_path}) {
        const auto updated_cache = load_ordered_cache(cpath);
        ASSERT_EQ(updated_cache.size(), 3u);
        EXPECT_EQ(updated_cache[0].first, "ゼット");
        EXPECT_EQ(updated_cache[0].second, "Z_value");
        EXPECT_EQ(updated_cache[1].second,
                  "Taichi\r\n\"Let's get the broadcasting club ready.\"");
        EXPECT_EQ(updated_cache[2].first, "エー");
        EXPECT_EQ(updated_cache[2].second, "A_value");
    }
}

// ============================================================================
// System Test 6: Literary Leeway Acceptance
// ============================================================================

TEST(SpotCheckerSystemTests, SystemTest6_LiteraryLeewayAcceptance) {
    // 1. Wooden literal line -> natural spoken English with syntactic leeway
    const ScriptEntry wooden_entry = make_entry(
        0, "Touko",
        "「脳割れてるんじゃない？」",
        "\"As for the brain, is it not being split?\"");
    const ValidationResult v1 = validate_proposed_fix(
        wooden_entry, "\"...Is your brain split in two?\"");
    EXPECT_TRUE(v1.accepted) << v1.rejection_reason;

    // 2. Punchy idiomatic banter callback with natural length difference
    const ScriptEntry callback_entry = make_entry(
        1, "Taichi",
        "「割れてる。」",
        "\"It is split.\"");
    const ValidationResult v2 = validate_proposed_fix(
        callback_entry, "\"Technically, yes.\"");
    EXPECT_TRUE(v2.accepted) << v2.rejection_reason;

    // Also verify default_editorial_rules_prompt contains the Literary Sweet Spot guidance
    const std::string sys = default_editorial_rules_prompt();
    EXPECT_NE(sys.find("THE LITERARY SWEET SPOT"), std::string::npos);
    EXPECT_NE(sys.find("WOODEN MECHANICAL LITERALISM"), std::string::npos);
    EXPECT_NE(sys.find("TONE DRIFT & OVER-LOCALIZATION"), std::string::npos);
}

// ============================================================================
// System Test 7: Literature-Backed CUSUM Change-Point, Discourse Signals,
// Stage 1 Packed LLM Radar, and Stage 2 GEMBA-MQM Span-Grounded Repair
// ============================================================================

TEST(SpotCheckerSystemTests, SystemTest7_TwoStageHybridRadarAndCusumChangepoint) {
    // 1. Verify Gale & Church (1993) + Pellegrino et al. (2011) script-aware information bits:
    //    Kanji (2.4 bits) > Katakana (1.4 bits) > Hiragana (1.0 bit).
    const double bits_kanji = compute_script_info_bits("脳割");
    const double bits_kata = compute_script_info_bits("テスト");
    const double bits_hira = compute_script_info_bits("あいう");
    EXPECT_NEAR(bits_kanji, 4.8, 1e-6);
    EXPECT_NEAR(bits_kata, 4.2, 1e-6);
    EXPECT_NEAR(bits_hira, 3.0, 1e-6);

    // 2. Verify Document-Level JP->EN Discourse Trap Signals & Page's CUSUM (1954):
    //    Build a 120-line novel where lines 40..49 suffer a sudden regime shift of:
    //    - Directional benefactive reversal (`てくれた` translated as "I helped...")
    //    - Overt 1st-person pronoun contradiction (`俺は` translated with zero 1st-person)
    //    - QA adjacency verb-echo disconnect (`脳割れてるんじゃない？` -> `割れてる。`)
    //    - Yule's K content-word repetition degeneracy
    std::vector<ScriptEntry> entries;
    for (std::size_t i = 0; i < 120; ++i) {
        entries.push_back(make_entry(
            i, "Taichi",
            "「静かな朝だな。今日も風が心地よい。」",
            "\"It's a quiet morning. The breeze feels pleasant today too.\""));
    }
    // Inject discourse trap cluster at 40..45
    entries[40] = make_entry(
        40, "Taichi",
        "「彼女が俺を助けてくれたのだ。」",
        "\"I helped her out of trouble.\"");  // Benefactive reversal!
    entries[41] = make_entry(
        41, "Taichi",
        "「俺は絶対に諦めないぞ。」",
        "\"They will never give up on that.\"");  // Overt 1st-person dropped!
    entries[42] = make_entry(
        42, "Touko",
        "「脳割れてるんじゃない？」",
        "\"Is your head split open?\"");
    entries[43] = make_entry(
        43, "Taichi",
        "「割れてる。」",
        "\"Affirmative.\"");  // Broken QA verb-echo callback!
    entries[44] = make_entry(
        44, "Miki",
        "「どうしてそんなこと言うかにゃ？」",
        "\"Why would you say such a thing?\"");  // Kinsui Yakuwarigo cat-tic dropped!
    entries[45] = make_entry(
        45, "Taichi",
        "「その言葉を聞いて安心した。」",
        "\"As for the matter of that word, the word of the matter was a word of relief.\"");

    HotspotConfig hcfg;
    hcfg.window_size = 20;
    hcfg.z_threshold = 1.5;
    HotspotDetector detector(hcfg);

    std::vector<LineWeaknessScore> scores;
    const auto hotspots = detector.select_hotspots(entries, 5, &scores);
    ASSERT_FALSE(hotspots.empty());
    EXPECT_GT(scores[40].coupling_score, 0.0);  // Benefactive reversal flagged
    EXPECT_GT(scores[41].coupling_score, 0.0);  // Overt 1st-person contradiction flagged
    EXPECT_GT(scores[43].coupling_score, 0.0);  // QA verb-echo mismatch flagged
    EXPECT_GT(scores[44].coupling_score, 0.0);  // Yakuwarigo drop flagged
    EXPECT_GT(scores[45].style_score, 0.0);     // Translationese + Yule's K repetition flagged
    // Verify Page's CUSUM accumulated a positive change-point peak over the regime shift
    EXPECT_GT(hotspots.front().cusum_peak, 0.0);
    EXPECT_LE(hotspots.front().offset, 45u);
    EXPECT_GE(hotspots.front().offset + hotspots.front().length, 41u);

    // 3. Verify Stage 2 GEMBA-MQM `error_span` enforcement in `validate_proposed_fix`:
    //    If the LLM specifies `error_span: "I helped her"`, a candidate fix that still
    //    contains `"I helped her"` MUST be rejected!
    const ValidationResult bad_span_fix = validate_proposed_fix(
        entries[40],
        "\"I helped her out of the crisis.\"",
        {},
        {},
        "I helped her");
    EXPECT_FALSE(bad_span_fix.accepted);
    EXPECT_NE(bad_span_fix.rejection_reason.find("error_span"), std::string::npos);

    const ValidationResult good_span_fix = validate_proposed_fix(
        entries[40],
        "\"She was the one who saved me.\"",
        {},
        {},
        "I helped her");
    EXPECT_TRUE(good_span_fix.accepted) << good_span_fix.rejection_reason;

    // 4. Verify End-to-End Two-Stage Hybrid Pipeline (`radar_calls = 1` + Stage 2 repair):
    TempDir tmp;
    bj::array strings_arr;
    for (const auto& e : entries) {
        strings_arr.push_back(bj::object{
            {"index", static_cast<std::int64_t>(e.entry_index)},
            {"type", "dialogue"},
            {"speaker", e.speaker},
            {"text", e.jp_text},
            {"translated", e.current_en},
        });
    }
    const bj::object root_obj{
        {"sn.bin", bj::object{{"strings", std::move(strings_arr)}}}};
    write_file((tmp.path() / "script_output" / "translated_text.json").u8string(),
               json_pretty(bj::value(root_obj), 2));

    SpotCheckConfig cfg;
    cfg.game_dir = tmp.str();
    cfg.mode = SpotCheckMode::Hotspot;
    cfg.window_size = 20;
    cfg.z_threshold = 1.5;
    cfg.radar_calls = 1;
    cfg.max_windows = 1;

    std::size_t radar_invocations = 0;
    std::size_t stage2_invocations = 0;

    SpotChecker checker(cfg, [&](const std::string&, const std::string& user_prompt) {
        if (user_prompt.find("<radar_triage_mandate>") != std::string::npos) {
            ++radar_invocations;
            return R"([
                {"passage_id": 1, "mqm_score": 35, "suspect_ids": [11, 14]}
            ])";
        }
        ++stage2_invocations;
        // Return two candidates:
        // - One that repairs the offending `error_span` (ACCEPTED)
        // - One that fails to remove its declared `error_span` (REJECTED)
        return R"json([
            {
                "id": 11,
                "new_en": "\"She was the one who saved me.\"",
                "error_span": "I helped her",
                "severity": "critical",
                "category": "mistranslation",
                "confidence": 0.96,
                "reason": "Fix benefactive direction (te-kureta = she saved me)"
            },
            {
                "id": 14,
                "new_en": "\"Affirmative, indeed.\"",
                "error_span": "Affirmative",
                "severity": "major",
                "category": "broken_callback",
                "confidence": 0.92,
                "reason": "Bad fix that left error_span 'Affirmative' intact"
            }
        ])json";
    });

    const SpotCheckReport rep = checker.run();
    EXPECT_EQ(radar_invocations, 1u);
    EXPECT_EQ(stage2_invocations, 1u);
    EXPECT_EQ(rep.radar_calls_made, 1u);
    EXPECT_GE(rep.radar_passages_triaged, 1u);
    EXPECT_EQ(rep.candidates_rejected_by_validator, 1u);
    ASSERT_EQ(rep.improvements_found, 1u);
    EXPECT_EQ(rep.improvements[0].severity, "critical");
    EXPECT_EQ(rep.improvements[0].error_span, "I helped her");
    EXPECT_EQ(rep.improvements[0].improved_en, "\"She was the one who saved me.\"");
}

// ============================================================================
// System Test 8: H-Scene Exclusion & Direct Auto-Apply into JSON
// ============================================================================

TEST(SpotCheckerSystemTests, SystemTest8_HSceneExclusionAndAutoApplyDirectlyInJson) {
    // 1. Verify is_h_scene_line detects JP and EN H-scene markers and ignores normal prose
    EXPECT_TRUE(is_h_scene_line("「んっ……熱くて硬い肉棒が秘部に……」", "\"Nngh...\""));
    EXPECT_TRUE(is_h_scene_line("「ああっ」", "\"His cock was thrusting deep inside.\""));
    EXPECT_FALSE(is_h_scene_line("「屋上のアンテナを修理しよう」",
                                 "\"Let's repair the cockpit antenna document.\""));

    // 2. Build a 100-line novel where:
    //    - Window [10..29] is a heavy H-scene containing 8 explicit H-lines AND 4 non-H
    //      lines with "(hippu)" romaji notes (total window >= 3 H-lines -> MUST be skipped!)
    //    - Window [70..89] is a normal non-H scene with a real defect at index 75.
    TempDir tmp;
    bj::array strings_arr;
    for (std::size_t i = 0; i < 100; ++i) {
        std::string jp = "「屋上のアンテナは静かに風を受けていた。」";
        std::string en = "\"The antenna on the rooftop quietly caught the breeze.\"";
        if (i >= 12 && i <= 19) {
            jp = "「んっ……熱い肉棒が秘部の奥まで挿入されて絶頂しちゃう……」";
            en = "\"Nngh... his hot cock is thrusting to 絶頂 climax...\"";
        } else if (i >= 20 && i <= 23) {
            jp = "「体が熱い……」";
            en = "\"As for my body, my 身体 is hot...\"";
        } else if (i == 75) {
            jp = "「見里先輩は屋上にいる」";
            en = "\"As for Misato-senpai, she is on the 屋上 roof.\"";
        }
        strings_arr.push_back(bj::object{
            {"index", static_cast<std::int64_t>(i)},
            {"type", "dialogue"},
            {"speaker", "Taichi"},
            {"text", jp},
            {"translated", en},
        });
    }
    const std::string translated_path =
        (tmp.path() / "script_output" / "translated_text.json").u8string();
    write_file(translated_path,
               json_pretty(bj::value(bj::object{
                               {"sn.bin", bj::object{{"strings", std::move(strings_arr)}}}}),
                           2));

    const std::string cache_path =
        (tmp.path() / "script_output" / "translation_cache_anthropic.json").u8string();
    save_ordered_cache(
        {{"「見里先輩は屋上にいる」", "\"As for Misato-senpai, she is on the 屋上 roof.\""}},
        cache_path);

    SpotCheckConfig cfg;
    cfg.game_dir = tmp.str();
    cfg.mode = SpotCheckMode::Hotspot;
    cfg.window_size = 20;
    cfg.context_before = 0;
    cfg.context_after = 0;
    cfg.max_windows = 1;
    // skip_h_scenes and auto_apply default to true, fix_punctuation defaults to false!
    EXPECT_TRUE(cfg.skip_h_scenes);
    EXPECT_TRUE(cfg.auto_apply);
    EXPECT_FALSE(cfg.fix_punctuation);

    SpotChecker checker(cfg, [&](const std::string&, const std::string& user_prompt) {
        // Ensure the H-scene lines were NEVER sent to the LLM
        EXPECT_EQ(user_prompt.find("肉棒"), std::string::npos);
        // Find which 1-based target ID corresponds to line 75 ("見里先輩は屋上にいる")
        for (int id = 1; id <= 20; ++id) {
            const std::string tag = std::to_string(id) + ". [Taichi]";
            const std::size_t pos = user_prompt.find(tag);
            if (pos == std::string::npos) continue;
            const std::size_t next_pos =
                user_prompt.find(std::to_string(id + 1) + ". [", pos);
            const std::string block = user_prompt.substr(
                pos, next_pos == std::string::npos ? 240 : next_pos - pos);
            if (block.find("見里先輩は屋上にいる") != std::string::npos) {
                return bj::serialize(bj::array{bj::object{
                    {"id", id},
                    {"new_en", "\"Misato-senpai is on the roof.\""},
                    {"category", "artifact_or_note"},
                    {"confidence", 0.98},
                    {"reason", "Remove leaked kanji and stiff calque"},
                }});
            }
        }
        return std::string("[]");
    });

    const SpotCheckReport rep = checker.run();
    ASSERT_FALSE(rep.ranked_hotspots.empty());
    // Top hotspot MUST cover non-H defect index 75 and completely avoid H-scene [12..23]
    EXPECT_LE(rep.ranked_hotspots.front().offset, 75u);
    EXPECT_GT(rep.ranked_hotspots.front().offset, 23u);
    ASSERT_EQ(rep.improvements_found, 1u);
    EXPECT_EQ(rep.script_entries_updated, 1u);
    EXPECT_EQ(rep.cache_entries_updated, 1u);

    // Verify translated_text.json and translation_cache_anthropic.json were updated
    // DIRECTLY by checker.run() without calling apply_improvements()!
    const bj::value updated_root = json_parse_file(translated_path);
    const auto& row75 =
        updated_root.as_object().at("sn.bin").as_object().at("strings").as_array()[75].as_object();
    EXPECT_EQ(row75.at("translated").as_string(), "\"Misato-senpai is on the roof.\"");

    const auto updated_cache = load_ordered_cache(cache_path);
    ASSERT_EQ(updated_cache.size(), 1u);
    EXPECT_EQ(updated_cache[0].second, "\"Misato-senpai is on the roof.\"");
}

// ============================================================================
// System Test 9: has_real_japanese Gate, Passthrough Protection &
// Punctuation/Parenthesis Noise Filter (fix_punctuation = false by default)
// ============================================================================

TEST(SpotCheckerSystemTests, SystemTest9_PassthroughSymbolGateAndPunctuationNoiseFilter) {
    // 1. Verify has_real_japanese(jp) matches CROSS_CHANNEL's cache_core.cpp rule:
    //    Symbol-only / fullwidth ASCII / punctuation lines have NO Hiragana/Katakana/Kanji
    EXPECT_FALSE(has_real_japanese("「？」"));
    EXPECT_FALSE(has_real_japanese("「……」"));
    EXPECT_FALSE(has_real_japanese("「！」"));
    EXPECT_FALSE(has_real_japanese("「……４Ｂ＠：……」"));
    EXPECT_FALSE(has_real_japanese("「６……５……４……」"));
    EXPECT_TRUE(has_real_japanese("「おはよう」"));
    EXPECT_TRUE(has_real_japanese("「テスト」"));
    EXPECT_TRUE(has_real_japanese("「屋上」"));

    // 2. Verify HotspotDetector gives 0.0 score to symbol-only passthrough lines AND
    //    ignores parenthetical/punctuation noise when fix_punctuation == false (default)
    HotspotDetector default_det;
    const LineWeaknessScore sym_q =
        default_det.score_single_line("太一\r\n「？」", "Taichi\r\n「？」", {});
    const LineWeaknessScore sym_dots =
        default_det.score_single_line("冬子\r\n「……」", "Touko\r\n「……」", {});
    const LineWeaknessScore sym_code =
        default_det.score_single_line("「……４Ｂ＠：……」", "「……４Ｂ＠：……」", {});
    EXPECT_DOUBLE_EQ(sym_q.raw_total, 0.0);
    EXPECT_DOUBLE_EQ(sym_dots.raw_total, 0.0);
    EXPECT_DOUBLE_EQ(sym_code.raw_total, 0.0);

    // Parenthetical romaji note "(hippu)" is NOT scored when fix_punctuation == false,
    // but IS scored when fix_punctuation == true:
    const LineWeaknessScore note_default = default_det.score_single_line(
        "「偉大なヒップだ」", "\"What a glorious hip (hippu).\"", {});
    EXPECT_DOUBLE_EQ(note_default.artifact_score, 0.0);

    HotspotConfig punc_cfg;
    punc_cfg.fix_punctuation = true;
    HotspotDetector punc_det(punc_cfg);
    const LineWeaknessScore note_enabled = punc_det.score_single_line(
        "「偉大なヒップだ」", "\"What a glorious hip (hippu).\"", {});
    EXPECT_GT(note_enabled.artifact_score, 0.0);

    // 3. Verify compute_editable_ids NEVER unlocks symbol-only or passthrough lines
    //    even if review_all_in_window == true!
    std::vector<ScriptEntry> entries = {
        make_entry(0, "Touko", "冬子\r\n「？」", "Touko\r\n「？」"),
        make_entry(1, "Taichi", "太一\r\n「……」", "Taichi\r\n「……」"),
        make_entry(2, "Taichi", "太一\r\n「おはよう」", "Taichi\r\n\"Good morning.\""),
    };
    WindowSlice slice = build_window_slice(entries, 0, 3, 0, 0);
    const std::set<int> editable =
        compute_editable_ids(slice, {}, /*review_all_in_window=*/true, /*skip_h_scenes=*/true);
    EXPECT_EQ(editable.count(1), 0u);
    EXPECT_EQ(editable.count(2), 0u);
    EXPECT_EQ(editable.count(3), 1u);

    // 4. Verify validate_proposed_fix blocks edits to symbol-only/passthrough lines AND
    //    blocks punctuation-only changes ("Hah---" -> "Hah--") when fix_punctuation == false
    const ValidationResult v_passthrough =
        validate_proposed_fix(entries[0], "Touko\r\n\"?\"");
    EXPECT_FALSE(v_passthrough.accepted);

    const ScriptEntry dash_entry = make_entry(
        3, "Taichi", "太一\r\n「はぁーーーっ」", "Taichi\r\n\"Hah---\"");
    const ValidationResult v_punc_default = validate_proposed_fix(
        dash_entry, "Taichi\r\n\"Hah--\"", {}, {}, "", /*fix_punctuation=*/false);
    EXPECT_FALSE(v_punc_default.accepted);
    EXPECT_NE(v_punc_default.rejection_reason.find("punctuation-only"), std::string::npos);

    const ValidationResult v_punc_enabled = validate_proposed_fix(
        dash_entry, "Taichi\r\n\"Hah--\"", {}, {}, "", /*fix_punctuation=*/true);
    EXPECT_TRUE(v_punc_enabled.accepted) << v_punc_enabled.rejection_reason;
}

// ============================================================================
// System Test 10: Multi-Run checked_offsets Persistence, repaired_indices Lock,
// Zero-Threshold Hotspot Queueing, and Phonetic Pun Prompt Rule
// ============================================================================

TEST(SpotCheckerSystemTests, SystemTest10_MultiRunCheckedOffsetsAndRepairedLineLocking) {
    // 1. Verify phonetic pun preservation rule is present in default_editorial_rules_prompt
    const std::string rules = default_editorial_rules_prompt(false);
    EXPECT_NE(rules.find("DO NOT strip the pun setup"), std::string::npos);

    // 2. Build a 40-line script with two separate 10-line windows containing defects:
    //    - Window A (around index 5) has 2 leaked kanji defects (stronger hotspot #1)
    //    - Window B (around index 28) has 1 leaked kanji defect (weaker hotspot #2, z_score < 0)
    TempDir tmp;
    bj::array strings_arr;
    for (int i = 0; i < 40; ++i) {
        std::string jp = "「屋上のアンテナは静かに風を受けていた。」";
        std::string en = "\"The antenna on the rooftop quietly caught the breeze.\"";
        if (i == 5 || i == 6) {
            jp = "「放送部の機材を屋上へ運ぼう」";
            en = "\"As for that, let's carry the 放送部 equipment to the 屋上.\"";
        } else if (i == 28) {
            jp = "「見里先輩が部室で待っている」";
            en = "\"Misato-senpai is waiting in the 部室 clubroom.\"";
        }
        strings_arr.push_back(bj::object{
            {"index", i},
            {"type", "dialogue"},
            {"speaker", "Taichi"},
            {"text", jp},
            {"translated", en},
        });
    }
    const std::string translated_path =
        (tmp.path() / "script_output" / "translated_text.json").u8string();
    write_json_preserving_style(
        translated_path,
        bj::value(bj::object{{"sn.bin", bj::object{{"strings", std::move(strings_arr)}}}}),
        1,
        true);

    SpotCheckConfig cfg;
    cfg.game_dir = tmp.str();
    cfg.mode = SpotCheckMode::Hotspot;
    cfg.window_size = 10;
    cfg.context_before = 0;
    cfg.context_after = 0;
    cfg.z_threshold = 0.0;  // Allow all non-H windows with energy > 0.0
    cfg.max_windows = 1;    // Check 1 window per run

    // Run 1: Checks Hotspot #1 (covering indices 5..6) and saves checked_offsets
    std::size_t run1_offset = 999;
    {
        SpotChecker run1(cfg, [&](const std::string&, const std::string& user_prompt) {
            for (int id = 1; id <= 10; ++id) {
                const std::string tag = std::to_string(id) + ". [Taichi]";
                const std::size_t pos = user_prompt.find(tag);
                if (pos == std::string::npos) continue;
                const std::size_t next_pos =
                    user_prompt.find(std::to_string(id + 1) + ". [", pos);
                const std::string block = user_prompt.substr(
                    pos, next_pos == std::string::npos ? 240 : next_pos - pos);
                if (block.find("放送部の機材") != std::string::npos) {
                    return bj::serialize(bj::array{bj::object{
                        {"id", id},
                        {"new_en", "\"Let's carry the broadcasting club equipment to the roof.\""},
                        {"category", "artifact_or_note"},
                        {"confidence", 0.97},
                        {"reason", "Fix leaked kanji"},
                    }});
                }
            }
            return std::string("[]");
        });
        const SpotCheckReport rep1 = run1.run();
        ASSERT_GE(rep1.hotspots_detected, 2u);  // z_threshold = 0.0 queued both windows!
        EXPECT_EQ(rep1.windows_checked, 1u);
        ASSERT_EQ(rep1.checked_offsets.size(), 1u);
        run1_offset = rep1.checked_offsets.front();
        EXPECT_LE(run1_offset, 5u);
        EXPECT_EQ(rep1.improvements_found, 1u);
    }

    // Run 2: Must load checked_offsets from argon_spot_check.json, skip run1_offset,
    // and check Hotspot #2 (covering index 28) instead!
    {
        SpotChecker run2(cfg, [&](const std::string&, const std::string& user_prompt) {
            // Run 2 must NOT re-send Hotspot #1's defect line
            EXPECT_EQ(user_prompt.find("放送部の機材"), std::string::npos);
            for (int id = 1; id <= 10; ++id) {
                const std::string tag = std::to_string(id) + ". [Taichi]";
                const std::size_t pos = user_prompt.find(tag);
                if (pos == std::string::npos) continue;
                const std::size_t next_pos =
                    user_prompt.find(std::to_string(id + 1) + ". [", pos);
                const std::string block = user_prompt.substr(
                    pos, next_pos == std::string::npos ? 240 : next_pos - pos);
                if (block.find("見里先輩が部室で待っている") != std::string::npos) {
                    return bj::serialize(bj::array{bj::object{
                        {"id", id},
                        {"new_en", "\"Misato-senpai is waiting in the clubroom.\""},
                        {"category", "artifact_or_note"},
                        {"confidence", 0.98},
                        {"reason", "Fix leaked kanji"},
                    }});
                }
            }
            return std::string("[]");
        });
        const SpotCheckReport rep2 = run2.run();
        EXPECT_EQ(rep2.windows_checked, 1u);
        EXPECT_EQ(rep2.checked_offsets.size(), 2u);
        EXPECT_EQ(rep2.improvements_found, 2u);  // Merged Run 1 + Run 2 improvements!
    }

    // 3. Also verify compute_editable_ids locks repaired_indices even when review_all_in_window=true
    std::vector<ScriptEntry> slice_entries = {
        make_entry(5, "Taichi", "「放送部の機材を屋上へ運ぼう」",
                   "\"Let's carry the broadcasting club equipment to the roof.\""),
        make_entry(6, "Taichi", "「見里先輩が部室で待っている」",
                   "\"Misato-senpai is waiting in the 部室 clubroom.\""),
    };
    const WindowSlice ws = build_window_slice(slice_entries, 0, 2, 0, 0);
    const std::set<std::size_t> repaired_set = {5};
    const std::set<int> unlocked = compute_editable_ids(
        ws, {}, /*review_all_in_window=*/true, /*skip_h_scenes=*/true, repaired_set);
    EXPECT_EQ(unlocked.count(1), 0u);  // global_index 5 is locked because it was already repaired!
    EXPECT_EQ(unlocked.count(2), 1u);
}
