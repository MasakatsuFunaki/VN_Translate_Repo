// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "detector/hotspot_detector.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace spot_checker {
namespace {

constexpr double kFloatTol = 1e-6;

ScriptEntry make_entry(std::size_t index,
                       std::string_view script_name,
                       std::string_view speaker,
                       std::string_view jp_text,
                       std::string_view current_en) {
    ScriptEntry entry;
    entry.global_index = index;
    entry.script_name = std::string(script_name);
    entry.array_key = "lines";
    entry.entry_index = index;
    entry.type = "TEXT";
    entry.text_key = "text";
    entry.speaker = std::string(speaker);
    entry.jp_text = std::string(jp_text);
    entry.current_en = std::string(current_en);
    return entry;
}

Improvement make_improvement_at(std::size_t global_index) {
    Improvement imp;
    imp.global_index = global_index;
    imp.script_name = "sc01";
    imp.entry_index = global_index;
    imp.speaker = "NARRATION";
    imp.jp_text = "あいうえおかきくけこ";
    imp.old_en = "Old line text.";
    imp.improved_en = "Improved line text.";
    imp.category = "mistranslation";
    imp.severity = "major";
    imp.confidence = 0.95;
    imp.reason = "Fixed mistranslation.";
    return imp;
}

std::vector<ScriptEntry> build_clean_corpus(std::size_t count,
                                            std::string_view script_name = "sc01") {
    constexpr const char* kShortJp[] = {
        "かきくけこさしす",
        "せそなにぬねのは",
        "ひふへほまみむめ",
        "もらりるれろかき",
        "くけこさしすせそ",
        "なにぬねのはひふ",
        "へほまみむめもら",
    };
    constexpr const char* kLongJp[] = {
        "かきくけこさしすせそなにぬねのはひふ",
        "へほまみむめもらりるれろかきくけこさ",
        "しすせそなにぬねのはひふへほまみむめ",
        "もらりるれろかきくけこさしすせそなに",
        "ぬねのはひふへほまみむめもらりるれろ",
        "かきくけこなにぬねのまみむめもらりる",
        "さしすせそはひふへほらりるれろかきく",
    };
    constexpr const char* kAdjectives[] = {
        "Quiet", "Golden", "Silver", "Gentle", "Distant", "Warm", "Crisp",
        "Bright", "Calm", "Soft", "Misty", "Clear", "Deep", "Amber", "Fresh",
        "Cool", "Pale", "Faint", "Rich", "Wide", "Smooth", "Swift", "Light",
    };
    constexpr const char* kNouns[] = {
        "clouds", "leaves", "waves", "lanterns", "shadows", "breezes", "petals",
        "sparrows", "ripples", "footsteps", "bells", "banners", "branches",
        "streams", "meadows", "hilltops", "harbors", "bridges", "gardens",
        "valleys", "forests",
    };
    constexpr const char* kVerbs[] = {
        "drifted", "crossed", "touched", "warmed", "filled", "reached", "passed",
        "circled", "glowed", "rested", "moved", "shone", "lingered", "floated",
        "settled", "wandered", "gathered", "opened", "echoed",
    };
    constexpr const char* kPreps[] = {
        "near", "past", "along", "above", "below", "under", "around",
        "beyond", "inside", "behind", "across", "toward", "within",
    };
    constexpr const char* kMaterials[] = {
        "ancient", "wooden", "granite", "stone", "marble", "shaded", "sunny",
        "mossy", "narrow", "winding", "coastal", "rustic", "painted", "arched", "quiet",
    };
    constexpr const char* kPlaces[] = {
        "courtyard", "corridor", "station", "library", "balcony", "terrace",
        "pathway", "archway", "stairway", "pavilion", "chapel", "fountain",
        "orchard", "veranda", "shoreline", "ridgeline", "platform",
    };
    constexpr std::size_t kJpCount = sizeof(kShortJp) / sizeof(kShortJp[0]);
    constexpr std::size_t kAdjCount = sizeof(kAdjectives) / sizeof(kAdjectives[0]);
    constexpr std::size_t kNounCount = sizeof(kNouns) / sizeof(kNouns[0]);
    constexpr std::size_t kVerbCount = sizeof(kVerbs) / sizeof(kVerbs[0]);
    constexpr std::size_t kPrepCount = sizeof(kPreps) / sizeof(kPreps[0]);
    constexpr std::size_t kMatCount = sizeof(kMaterials) / sizeof(kMaterials[0]);
    constexpr std::size_t kPlaceCount = sizeof(kPlaces) / sizeof(kPlaces[0]);

    std::vector<ScriptEntry> corpus;
    corpus.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::string adj = kAdjectives[i % kAdjCount];
        const std::string noun = kNouns[i % kNounCount];
        const std::string verb = kVerbs[i % kVerbCount];
        if ((i % 2U) == 0U) {
            const std::string en = adj + " " + noun + " " + verb + ".";
            corpus.push_back(
                make_entry(i, script_name, "NARRATION", kShortJp[i % kJpCount], en));
            continue;
        }
        const std::string prep = kPreps[i % kPrepCount];
        const std::string mat = kMaterials[i % kMatCount];
        const std::string place = kPlaces[i % kPlaceCount];
        const std::string en =
            adj + " " + noun + " " + verb + " " + prep + " " + mat + " " + place + ".";
        corpus.push_back(
            make_entry(i, script_name, "NARRATION", kLongJp[i % kJpCount], en));
    }
    return corpus;
}

TEST(HotspotDetectorTest, CleanCorpusReturnsZeroHotspots) {
    constexpr std::size_t kCorpusSize = 60;
    const std::vector<ScriptEntry> clean_corpus = build_clean_corpus(kCorpusSize);

    HotspotDetector detector;
    std::vector<LineWeaknessScore> scores;
    const std::vector<HotspotWindow> hotspots =
        detector.select_hotspots(clean_corpus, &scores);

    ASSERT_EQ(scores.size(), kCorpusSize);
    for (std::size_t i = 0; i < scores.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_NEAR(scores[i].total_score, 0.0, kFloatTol);
        EXPECT_NEAR(scores[i].cusum_stat, 0.0, kFloatTol);
        EXPECT_NEAR(scores[i].smoothed_density, 0.0, kFloatTol);
    }
    EXPECT_EQ(hotspots.size(), 0U);

    // Even with a negative z_threshold, 0-energy windows must never produce a bogus hotspot.
    HotspotConfig permissive_cfg;
    permissive_cfg.z_threshold = -10.0;
    HotspotDetector moved_detector(permissive_cfg);
    HotspotDetector assigned_detector = std::move(moved_detector);
    const std::vector<HotspotWindow> permissive_hotspots =
        assigned_detector.select_hotspots(clean_corpus);
    EXPECT_EQ(permissive_hotspots.size(), 0U);
}

TEST(HotspotDetectorTest, ResetsCusumAtScriptBoundariesAndAppliesEndpointUnbiasedKde) {
    HotspotConfig cfg;
    cfg.window_size = 10;
    HotspotDetector detector(cfg);

    // N = 0 seam:
    EXPECT_EQ(detector.score_entries({}).size(), 0U);
    EXPECT_EQ(detector.select_hotspots({}).size(), 0U);

    // N = 1 seam: active kernel sum renormalization yields smoothed_density == total_score.
    const std::vector<ScriptEntry> single_defect = {
        make_entry(0, "sc01", "NARRATION", "あいうえおかきくけこ",
                   "Morning light touched 東京 hills."),
    };
    const std::vector<LineWeaknessScore> single_scores =
        detector.score_entries(single_defect);
    ASSERT_EQ(single_scores.size(), 1U);
    EXPECT_GE(single_scores[0].total_score, 8.0);
    EXPECT_NEAR(single_scores[0].smoothed_density, single_scores[0].total_score, kFloatTol);

    // Endpoint-unbiased KDE on a constant-defect 6-line script + CUSUM/KDE reset on script_b:
    const char* const kDisjointScriptAJp[] = {
        "かきくけこさしすせそ",
        "なにぬねのはひふへほ",
        "まみむめもらりるれろ",
        "カキクケコサシスセソ",
        "ナニヌネノハヒフヘホ",
        "マミムメモラリルレロ",
    };
    const char* const kVariedLeakedEn[] = {
        "Silver clouds drifted above 東京 harbor.",
        "Lanterns flickered softly beside 京都 bridge.",
        "Distant bells echoed across 奈良 valley.",
        "Wooden boats rested near 神戸 pier.",
        "Autumn wind swept through 札幌 station.",
        "Quiet footsteps crossed 仙台 plaza.",
    };
    constexpr std::size_t kScriptALen = 6;
    constexpr std::size_t kScriptBLen = 6;
    std::vector<ScriptEntry> two_scripts;
    for (std::size_t i = 0; i < kScriptALen; ++i) {
        two_scripts.push_back(make_entry(
            i, "script_a", "NARRATION", kDisjointScriptAJp[i], kVariedLeakedEn[i]));
    }
    const std::vector<ScriptEntry> clean_tail = build_clean_corpus(kScriptBLen, "script_b");
    for (std::size_t i = 0; i < kScriptBLen; ++i) {
        ScriptEntry row = clean_tail[i];
        row.global_index = kScriptALen + i;
        two_scripts.push_back(std::move(row));
    }

    OlsModel fitted;
    const std::vector<LineWeaknessScore> boundary_scores =
        detector.score_entries(two_scripts, &fitted);
    ASSERT_EQ(boundary_scores.size(), kScriptALen + kScriptBLen);

    // All 6 lines in script_a have identical raw score 8.0; endpoint-renormalized KDE
    // preserves 8.0 at line 0 and line 5 without ~50% boundary drop-off.
    EXPECT_NEAR(boundary_scores[0].total_score, 8.0, kFloatTol);
    EXPECT_NEAR(boundary_scores[0].smoothed_density, 8.0, kFloatTol);
    EXPECT_NEAR(boundary_scores[kScriptALen - 1U].total_score, 8.0, kFloatTol);
    EXPECT_NEAR(boundary_scores[kScriptALen - 1U].smoothed_density, 8.0, kFloatTol);
    EXPECT_GT(boundary_scores[kScriptALen - 1U].cusum_stat, 30.0);

    // At script_b start (index 6), both CUSUM and Gaussian KDE strictly reset/truncate.
    EXPECT_NEAR(boundary_scores[kScriptALen].cusum_stat, 0.0, kFloatTol);
    EXPECT_NEAR(boundary_scores[kScriptALen].smoothed_density, 0.0, kFloatTol);

    // Window-size seams N = W - 1 (9), W (10), W + 1 (11) for W = 10:
    struct SizeSeamCase {
        std::size_t corpus_size;
        std::size_t expected_window_len;
    };
    constexpr SizeSeamCase kSizeCases[] = {
        {9U, 9U},
        {10U, 10U},
        {11U, 10U},
    };
    for (const auto& sc : kSizeCases) {
        SCOPED_TRACE(sc.corpus_size);
        std::vector<ScriptEntry> seam_corpus = build_clean_corpus(sc.corpus_size);
        seam_corpus.back().current_en = "Broken line with 東京 leak.";
        const std::vector<HotspotWindow> seam_hotspots =
            detector.select_hotspots(seam_corpus);
        ASSERT_EQ(seam_hotspots.size(), 1U);
        EXPECT_EQ(seam_hotspots[0].length, sc.expected_window_len);
        EXPECT_LE(seam_hotspots[0].offset + seam_hotspots[0].length, sc.corpus_size);
    }
}

TEST(HotspotDetectorTest, RanksByCombinedZEnforcesNmsSeparationAndCapsMaxWindows) {
    constexpr std::size_t kCorpusSize = 120;
    std::vector<ScriptEntry> corpus = build_clean_corpus(kCorpusSize);

    // Moderate defect at 15 and weaker nearby defect at 22 (within nms_min_separation = 25).
    corpus[15].current_en = "Single leaked 東京 token in prose.";
    corpus[16].current_en = "Second leaked 大阪 token in prose.";
    corpus[22].current_en = "Nearby weaker 京都 leak suppressed by NMS.";

    // Dominant multi-line cluster at 85..89.
    for (std::size_t idx = 85; idx < 90; ++idx) {
        corpus[idx].current_en = "Severe cluster 学園祭 leak and runawayyyyyyyyyyy!";
    }

    HotspotConfig cfg;
    cfg.window_size = 10;
    cfg.nms_min_separation = 25;
    cfg.z_threshold = 0.0;
    HotspotDetector detector(cfg);

    const std::vector<HotspotWindow> hotspots = detector.select_hotspots(corpus);
    ASSERT_EQ(hotspots.size(), 2U);
    EXPECT_GT(hotspots[0].z_score, hotspots[1].z_score);
    EXPECT_LE(hotspots[0].offset, 85U);
    EXPECT_GE(hotspots[0].offset + hotspots[0].length, 90U);
    EXPECT_LE(hotspots[1].offset, 15U);
    EXPECT_GE(hotspots[1].offset + hotspots[1].length, 16U);

    const std::size_t separation = hotspots[0].offset > hotspots[1].offset
                                       ? (hotspots[0].offset - hotspots[1].offset)
                                       : (hotspots[1].offset - hotspots[0].offset);
    EXPECT_GE(separation, cfg.nms_min_separation);

    cfg.max_windows = 1;
    HotspotDetector capped_detector(cfg);
    const std::vector<HotspotWindow> capped = capped_detector.select_hotspots(corpus);
    ASSERT_EQ(capped.size(), 1U);
    EXPECT_EQ(capped[0].offset, hotspots[0].offset);
}

TEST(HotspotDetectorTest, EvaluatesContagionDefectRateBoundaryMarginsAndOverflowSafety) {
    HotspotConfig cfg;
    cfg.window_size = 40;
    cfg.contagion_defect_rate = 0.12;
    cfg.boundary_margin = 5;
    HotspotDetector detector(cfg);

    // Exact defect_rate seam: 119 / 1000 = 0.119 (< 0.12) vs 120 / 1000 = 0.120 (>= 0.12),
    // with out-of-window improvements ignored.
    constexpr std::size_t kRateOffset = 2000;
    constexpr std::size_t kRateLen = 1000;
    constexpr std::size_t kRateTotal = 5000;
    constexpr std::size_t kBelowRateFixes = 119;
    std::vector<Improvement> fixes_119;
    fixes_119.push_back(make_improvement_at(kRateOffset - 1U));
    fixes_119.push_back(make_improvement_at(kRateOffset + kRateLen));
    for (std::size_t i = 0; i < kBelowRateFixes; ++i) {
        fixes_119.push_back(make_improvement_at(kRateOffset + 100U + i));
    }

    const NeighborExpansion below_rate =
        detector.evaluate_contagion(kRateOffset, kRateLen, kRateTotal, fixes_119);
    EXPECT_FALSE(below_rate.expand_left);
    EXPECT_FALSE(below_rate.expand_right);

    std::vector<Improvement> fixes_120 = fixes_119;
    fixes_120.push_back(make_improvement_at(kRateOffset + 300U));
    const NeighborExpansion at_rate =
        detector.evaluate_contagion(kRateOffset, kRateLen, kRateTotal, fixes_120);
    EXPECT_TRUE(at_rate.expand_left);
    EXPECT_TRUE(at_rate.expand_right);

    // Boundary margin seams on [100, 140) with margin = 5 (1 fix = 2.5% < 12%):
    // Left edge: 104 (100 + 5 - 1) expands left; 105 (100 + 5) does not.
    const NeighborExpansion left_inside =
        detector.evaluate_contagion(100U, 40U, 300U, {make_improvement_at(104U)});
    EXPECT_TRUE(left_inside.expand_left);
    EXPECT_FALSE(left_inside.expand_right);
    EXPECT_EQ(left_inside.left_offset, 60U);
    EXPECT_EQ(left_inside.left_length, 40U);

    const NeighborExpansion left_outside =
        detector.evaluate_contagion(100U, 40U, 300U, {make_improvement_at(105U)});
    EXPECT_FALSE(left_outside.expand_left);
    EXPECT_FALSE(left_outside.expand_right);

    // Right edge: 134 (140 - 5 - 1) does not expand; 135 (140 - 5) expands right.
    const NeighborExpansion right_outside =
        detector.evaluate_contagion(100U, 40U, 300U, {make_improvement_at(134U)});
    EXPECT_FALSE(right_outside.expand_left);
    EXPECT_FALSE(right_outside.expand_right);

    const NeighborExpansion right_inside =
        detector.evaluate_contagion(100U, 40U, 300U, {make_improvement_at(135U)});
    EXPECT_FALSE(right_inside.expand_left);
    EXPECT_TRUE(right_inside.expand_right);
    EXPECT_EQ(right_inside.right_offset, 140U);
    EXPECT_EQ(right_inside.right_length, 40U);

    // Clamping & unsigned underflow/overflow safety:
    const NeighborExpansion at_zero =
        detector.evaluate_contagion(0U, 40U, 200U, {make_improvement_at(0U)});
    EXPECT_FALSE(at_zero.expand_left);
    EXPECT_EQ(at_zero.left_length, 0U);

    const NeighborExpansion clamped_left =
        detector.evaluate_contagion(15U, 40U, 200U, {make_improvement_at(15U)});
    EXPECT_TRUE(clamped_left.expand_left);
    EXPECT_EQ(clamped_left.left_offset, 0U);
    EXPECT_EQ(clamped_left.left_length, 15U);

    const NeighborExpansion clamped_right =
        detector.evaluate_contagion(150U, 40U, 205U, {make_improvement_at(189U)});
    EXPECT_TRUE(clamped_right.expand_right);
    EXPECT_EQ(clamped_right.right_offset, 190U);
    EXPECT_EQ(clamped_right.right_length, 15U);

    const std::size_t near_max = std::numeric_limits<std::size_t>::max() - 10U;
    const NeighborExpansion overflow_guard =
        detector.evaluate_contagion(near_max, 40U, 100U, {make_improvement_at(near_max)});
    EXPECT_FALSE(overflow_guard.expand_left);
    EXPECT_FALSE(overflow_guard.expand_right);
}

TEST(HotspotDetectorTest,
     TreatsSustainedHScenesAsHardBarriersWhileSurfacingShortScenesBanterAndCombat) {
    // 216-line multi-segment corpus (window_size = 20, nms_min_separation = 30):
    //   [0, 12)    script_main: 12-line (< window_size) opening scene with defects at 4..5
    //   [12, 36)   script_main: 24-line untagged interleaved moan + literary eroge H-scene
    //   [36, 80)   script_main: 44-line non-H segment with 4-line dirty banter defects at 52..55
    //   [80, 102)  script_main: 22-line pure moan + (Ｈ) tagged H-scene
    //   [102, 182) script_main: 80-line clean prose buffer
    //   [182, 216) script_combat: 26-line combat/chase panting scene (defects at 192..195) + 8 clean
    constexpr std::size_t kMainLines = 182;
    constexpr std::size_t kCombatScriptLines = 34;
    std::vector<ScriptEntry> corpus = build_clean_corpus(kMainLines, "script_main");

    // 1. Short (< window_size) opening scene defects right before H-barrier [12, 36):
    corpus[4].current_en = "Broken opening line with 東京 leak and runawayyyyyyyyyyy!";
    corpus[5].current_en = "Second opening defect with 大阪 leak and runawayyyyyyyyyyy!";

    // 2. Sustained 24-line interleaved moan + literary eroge H-scene [12, 36) (untagged speakers),
    // heavily seeded with defects on every line (13 erotic anchors + 11 bridged plain lines):
    for (std::size_t idx = 12; idx < 36; ++idx) {
        const bool is_anchor = ((idx - 12U) % 2U == 0U) || (idx == 35U);
        corpus[idx] = make_entry(
            idx, "script_main", is_anchor ? "Yui" : "NARRATION",
            is_anchor ? "「んっ……あんっ……熱い秘裂と蜜壺が愛液でぐちゅぐちゅ鳴る……♡」"
                      : "窓の外で夜の雨が静かに降り続いていた。",
            is_anchor ? "\"Nngh... schlick, wet folds in 快楽 runawayyyyyyyyyyy!\""
                      : "Night rain fell outside the 窓辺 window runawayyyyyyyyyyy!");
    }

    // 3. Short 4-line non-H dirty/crude banter cluster [52, 56) inside [36, 80):
    corpus[52] = make_entry(52U, "script_main", "Kenji", "「ふざけるな、予算を出せよ！」",
                            "\"Quit acting like a cock about the 東京 club budget!\"");
    corpus[53] = make_entry(53U, "script_main", "Yui", "「逃げる気じゃないでしょうね！」",
                            "\"Don't pussy out right before the 大阪 match!\"");
    corpus[54] = make_entry(54U, "script_main", "Kenji", "「さっさと駅へ向かうぞ！」",
                            "\"Move your sorry ass toward 京都 station now!\"");
    corpus[55] = make_entry(55U, "script_main", "Yui", "「言い訳ばかりしないでよ！」",
                            "\"Cut the dickhead excuses near 奈良 gate!\"");

    // 4. Sustained 22-line pure moan + (Ｈ)-tagged H-scene [80, 102), heavily defective:
    for (std::size_t idx = 80; idx < 102; ++idx) {
        const bool tagged = ((idx % 2U) == 1U);
        corpus[idx] = make_entry(
            idx, "script_main", tagged ? "美咲（Ｈ）" : "Yui",
            tagged ? "「もっと近くに来て……離さないで……」"
                   : "「んっ……んちゅ、ちゅぱ……じゅる……イッちゃう……！」",
            tagged ? "\"Come closer in 抱擁 runawayyyyyyyyyyy!\""
                   : "\"Nngh... slurp, squelch... 絶頂 runawayyyyyyyyyyy!\"");
    }

    // 5. Sustained 26-line combat/pain/chase panting scene in script_combat [182, 208) + 8 clean:
    const std::vector<ScriptEntry> combat_tail =
        build_clean_corpus(kCombatScriptLines, "script_combat");
    constexpr const char* kCombatWeapons[] = {
        "blade", "spear", "arrow", "dagger", "halberd", "steel", "lance",
    };
    constexpr const char* kCombatSpots[] = {
        "ridge", "alley", "bridge", "rampart", "bastion", "watchtower",
    };
    constexpr std::size_t kCombatPantingLines = 26;
    for (std::size_t i = 0; i < kCombatPantingLines; ++i) {
        ScriptEntry row = combat_tail[i];
        row.global_index = kMainLines + i;
        row.speaker = (i % 2U == 0U) ? "Kenji" : "Taro";
        row.jp_text = (i % 2U == 0U)
                          ? "「ぐあっ……くっ、剣の傷が痛む……走れ！」"
                          : "「はぁ、はぁ……敵の刃をかわして城門へ急げ！」";
        row.current_en = std::string("\"Gah! Huff, huff... dodge that ") +
                         kCombatWeapons[i % 7U] + " along the " +
                         kCombatSpots[i % 6U] + " wounded shoulder!\"";
        corpus.push_back(std::move(row));
    }
    for (std::size_t i = kCombatPantingLines; i < kCombatScriptLines; ++i) {
        ScriptEntry row = combat_tail[i];
        row.global_index = kMainLines + i;
        corpus.push_back(std::move(row));
    }
    for (std::size_t idx = 192; idx < 196; ++idx) {
        corpus[idx].current_en =
            "\"Gah! Huff, huff... retreat past 札幌 blade wound runawayyyyyyyyyyy!\"";
    }

    HotspotConfig skip_cfg;
    skip_cfg.window_size = 20;
    skip_cfg.nms_min_separation = 30;
    skip_cfg.z_threshold = 0.0;
    skip_cfg.skip_h_scenes = true;
    HotspotDetector skip_detector(skip_cfg);

    std::vector<HotspotWindow> skip_hotspots = skip_detector.select_hotspots(corpus);
    ASSERT_EQ(skip_hotspots.size(), 3U);
    std::sort(skip_hotspots.begin(), skip_hotspots.end(),
              [](const HotspotWindow& a, const HotspotWindow& b) {
                  return a.offset < b.offset;
              });

    // Window 0: short 12-line opening scene [0, 12), clamped before H-barrier [12, 36).
    EXPECT_EQ(skip_hotspots[0].offset, 0U);
    EXPECT_EQ(skip_hotspots[0].length, 12U);

    // Window 1: dirty banter [52..55] strictly inside non-H interval [36, 80).
    EXPECT_GE(skip_hotspots[1].offset, 36U);
    EXPECT_LE(skip_hotspots[1].offset + skip_hotspots[1].length, 80U);
    EXPECT_EQ(skip_hotspots[1].length, 20U);
    EXPECT_LE(skip_hotspots[1].offset, 52U);
    EXPECT_GE(skip_hotspots[1].offset + skip_hotspots[1].length, 56U);

    // Window 2: combat panting cluster [192..195] strictly after H-barrier [80, 102).
    EXPECT_GE(skip_hotspots[2].offset, 175U);
    EXPECT_LE(skip_hotspots[2].offset + skip_hotspots[2].length, 216U);
    EXPECT_EQ(skip_hotspots[2].length, 20U);
    EXPECT_LE(skip_hotspots[2].offset, 192U);
    EXPECT_GE(skip_hotspots[2].offset + skip_hotspots[2].length, 196U);

    // With skip_h_scenes = false, the two heavily defective sustained H-scenes dominate top-2.
    HotspotConfig include_cfg = skip_cfg;
    include_cfg.skip_h_scenes = false;
    include_cfg.max_windows = 2;
    HotspotDetector include_detector(include_cfg);

    std::vector<HotspotWindow> include_hotspots = include_detector.select_hotspots(corpus);
    ASSERT_EQ(include_hotspots.size(), 2U);
    std::sort(include_hotspots.begin(), include_hotspots.end(),
              [](const HotspotWindow& a, const HotspotWindow& b) {
                  return a.offset < b.offset;
              });
    EXPECT_LT(include_hotspots[0].offset, 36U);
    EXPECT_GT(include_hotspots[0].offset + include_hotspots[0].length, 12U);
    EXPECT_LT(include_hotspots[1].offset, 102U);
    EXPECT_GT(include_hotspots[1].offset + include_hotspots[1].length, 80U);
}

}  // namespace
}  // namespace spot_checker
