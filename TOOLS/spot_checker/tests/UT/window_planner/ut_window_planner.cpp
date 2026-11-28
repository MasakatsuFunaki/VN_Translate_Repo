// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "checker/window_planner.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace spot_checker {
namespace {

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

TEST(WindowPlannerTest, IntervalTrackerMergesSpansAndTrimsUncovered) {
    IntervalTracker tracker;

    tracker.mark_visited(50U, 0U);
    EXPECT_EQ(tracker.intervals().size(), 0U);
    EXPECT_TRUE(tracker.is_fully_covered(50U, 0U));
    EXPECT_EQ(tracker.trim_uncovered_span(50U, 0U), (std::pair<std::size_t, std::size_t>{50U, 0U}));

    tracker.mark_visited(100U, 40U);
    tracker.mark_visited(20U, 40U);
    ASSERT_EQ(tracker.intervals().size(), 2U);
    EXPECT_EQ(tracker.intervals()[0], (std::pair<std::size_t, std::size_t>{20U, 60U}));
    EXPECT_EQ(tracker.intervals()[1], (std::pair<std::size_t, std::size_t>{100U, 140U}));

    // Adjacent interval [60, 100) coalesces both [20, 60) and [100, 140) into [20, 140).
    tracker.mark_visited(60U, 40U);
    ASSERT_EQ(tracker.intervals().size(), 1U);
    EXPECT_EQ(tracker.intervals()[0], (std::pair<std::size_t, std::size_t>{20U, 140U}));

    EXPECT_TRUE(tracker.is_fully_covered(30U, 80U));
    EXPECT_EQ(tracker.trim_uncovered_span(30U, 80U), (std::pair<std::size_t, std::size_t>{30U, 0U}));

    // Partial left prefix covered: [20, 140) covers [100, 140) of [100, 165) -> {140, 25}.
    EXPECT_FALSE(tracker.is_fully_covered(100U, 65U));
    EXPECT_EQ(tracker.trim_uncovered_span(100U, 65U),
              (std::pair<std::size_t, std::size_t>{140U, 25U}));

    // Partial right suffix covered: [20, 140) covers [20, 45) of [5, 45) -> {5, 15}.
    EXPECT_FALSE(tracker.is_fully_covered(5U, 40U));
    EXPECT_EQ(tracker.trim_uncovered_span(5U, 40U),
              (std::pair<std::size_t, std::size_t>{5U, 15U}));

    // Both ends covered with uncovered middle gap [140, 170):
    tracker.mark_visited(170U, 30U);
    ASSERT_EQ(tracker.intervals().size(), 2U);
    EXPECT_EQ(tracker.trim_uncovered_span(120U, 70U),
              (std::pair<std::size_t, std::size_t>{140U, 30U}));

    // Overflow-safe span near SIZE_MAX:
    const std::size_t near_max = std::numeric_limits<std::size_t>::max() - 5U;
    tracker.mark_visited(near_max, 20U);
    EXPECT_TRUE(tracker.is_fully_covered(near_max, 5U));

    tracker.clear();
    EXPECT_EQ(tracker.intervals().size(), 0U);
}

TEST(WindowPlannerTest, BuildsWindowSliceAtZeroInteriorTailAndOutOfBounds) {
    constexpr std::size_t kCorpusSize = 12;
    std::vector<ScriptEntry> entries;
    entries.reserve(kCorpusSize);
    for (std::size_t i = 0; i < kCorpusSize; ++i) {
        entries.push_back(make_entry(i, "NARRATION", "あいうえおかきくけこ", "Translated line."));
    }

    // Offset 0: zero context_before, 4 target, 3 context_after.
    const WindowSlice at_zero = build_window_slice(entries, 0U, 4U, 3U, 3U);
    EXPECT_EQ(at_zero.target_start, 0U);
    EXPECT_EQ(at_zero.context_before.size(), 0U);
    ASSERT_EQ(at_zero.target.size(), 4U);
    EXPECT_EQ(at_zero.target.front().global_index, 0U);
    EXPECT_EQ(at_zero.target.back().global_index, 3U);
    ASSERT_EQ(at_zero.context_after.size(), 3U);
    EXPECT_EQ(at_zero.context_after.front().global_index, 4U);
    EXPECT_EQ(at_zero.context_after.back().global_index, 6U);

    // Interior offset 5: context_before [2..4], target [5..8], context_after [9..10].
    const WindowSlice interior = build_window_slice(entries, 5U, 4U, 3U, 2U);
    EXPECT_EQ(interior.target_start, 5U);
    ASSERT_EQ(interior.context_before.size(), 3U);
    EXPECT_EQ(interior.context_before.front().global_index, 2U);
    EXPECT_EQ(interior.context_before.back().global_index, 4U);
    ASSERT_EQ(interior.target.size(), 4U);
    EXPECT_EQ(interior.target.front().global_index, 5U);
    EXPECT_EQ(interior.target.back().global_index, 8U);
    ASSERT_EQ(interior.context_after.size(), 2U);
    EXPECT_EQ(interior.context_after.front().global_index, 9U);
    EXPECT_EQ(interior.context_after.back().global_index, 10U);

    // Tail offset + W > N (9 + 5 > 12): target clamped to [9..11], context_after empty.
    const WindowSlice tail = build_window_slice(entries, 9U, 5U, 4U, 4U);
    EXPECT_EQ(tail.target_start, 9U);
    ASSERT_EQ(tail.context_before.size(), 4U);
    EXPECT_EQ(tail.context_before.front().global_index, 5U);
    ASSERT_EQ(tail.target.size(), 3U);
    EXPECT_EQ(tail.target.front().global_index, 9U);
    EXPECT_EQ(tail.target.back().global_index, 11U);
    EXPECT_EQ(tail.context_after.size(), 0U);

    // Out-of-bounds offset >= N and window_size == 0 return empty slices.
    EXPECT_EQ(build_window_slice(entries, kCorpusSize, 4U, 2U, 2U).target.size(), 0U);
    EXPECT_EQ(build_window_slice(entries, 2U, 0U, 2U, 2U).target.size(), 0U);

    // Sustained H-scene mask clips context_before and context_after at nearest masked boundary.
    std::vector<bool> h_mask(kCorpusSize, false);
    h_mask[2U] = true;
    h_mask[10U] = true;
    const WindowSlice clipped = build_window_slice(entries, 5U, 4U, 4U, 3U, &h_mask);
    ASSERT_EQ(clipped.context_before.size(), 2U);
    EXPECT_EQ(clipped.context_before.front().global_index, 3U);
    EXPECT_EQ(clipped.context_before.back().global_index, 4U);
    ASSERT_EQ(clipped.target.size(), 4U);
    EXPECT_EQ(clipped.target.front().global_index, 5U);
    EXPECT_EQ(clipped.target.back().global_index, 8U);
    ASSERT_EQ(clipped.context_after.size(), 1U);
    EXPECT_EQ(clipped.context_after.front().global_index, 9U);

    // Immediate H-scene adjacency yields zero surrounding context on both sides.
    h_mask[4U] = true;
    h_mask[9U] = true;
    const WindowSlice flush_clipped = build_window_slice(entries, 5U, 4U, 4U, 3U, &h_mask);
    EXPECT_EQ(flush_clipped.context_before.size(), 0U);
    ASSERT_EQ(flush_clipped.target.size(), 4U);
    EXPECT_EQ(flush_clipped.context_after.size(), 0U);
}

TEST(WindowPlannerTest, ComputesEditableIdsWithScoreThresholdBanterPartnersAndLocks) {
    // 9 target rows at global_index 10..18 (1-based IDs 1..9):
    //   ID 1 (10): clean JP (score 0.0)
    //   ID 2 (11): near-threshold (score 1.79 < 1.80 -> not suspect)
    //   ID 3 (12): clean JP (score 0.0 -> unlocked as -1 banter partner of ID 4)
    //   ID 4 (13): unmasked dirty/crude banter at exact threshold (score 1.80 -> unlocked!)
    //   ID 5 (14): passthrough JP == EN (locked even as +1 partner of ID 4)
    //   ID 6 (15): sustained H-masked row (locked when skip_h_scenes=true && masked; unlocked otherwise)
    //   ID 7 (16): high-score suspect (score 3.5 -> suspect)
    //   ID 8 (17): already repaired in repaired_indices (locked even as +1 partner of ID 7)
    //   ID 9 (18): pure silence "「……」" (always locked)
    WindowSlice slice;
    slice.target_start = 10U;
    slice.target = {
        make_entry(10U, "Kenji", "「いい天気だな。」", "\"Nice weather today.\""),
        make_entry(11U, "Yui", "「そうね、散歩しよう。」", "\"Yes, let's take a walk.\""),
        make_entry(12U, "Kenji", "「駅前の喫茶店はどうだ？」", "\"How about the station cafe?\""),
        make_entry(13U, "Yui", "「ふざけないで、珈琲を淹れなさいよ。」",
                   "\"Quit being such a cock and brew the coffee already.\""),
        make_entry(14U, "NARRATION", "未翻訳のままの行", "未翻訳のままの行"),
        make_entry(15U, "美咲（Ｈ）", "「もっと近くに来て……」", "\"Come closer to me...\""),
        make_entry(16U, "Kenji", "「約束は必ず守るぞ。」", "\"That promise stands firm.\""),
        make_entry(17U, "Yui", "「信じているからね。」", "\"I believe in you completely.\""),
        make_entry(18U, "Yui", "「……」", "\"...\""),
    };

    constexpr std::size_t kTotalCorpus = 20;
    std::vector<LineWeaknessScore> scores(kTotalCorpus);
    for (std::size_t i = 0; i < kTotalCorpus; ++i) {
        scores[i].global_index = i;
    }
    scores[11U].total_score = 1.79;
    scores[13U].total_score = 1.80;
    scores[16U].total_score = 3.50;

    const std::set<std::size_t> repaired = {17U};
    std::vector<bool> h_scene_mask(kTotalCorpus, false);
    h_scene_mask[15U] = true;  // Sustained H-scene line; dirty banter at 13U stays false.

    const std::set<int> suspect_skip_h = compute_editable_ids(
        slice, scores, /*review_all_in_window=*/false, /*skip_h_scenes=*/true, repaired,
        &h_scene_mask);
    const std::set<int> expected_suspect_skip_h = {3, 4, 7};
    EXPECT_EQ(suspect_skip_h, expected_suspect_skip_h);

    // Without a sustained H-scene mask (nullptr), single-line H tags or dirty words never lock rows.
    const std::set<int> suspect_unmasked = compute_editable_ids(
        slice, scores, /*review_all_in_window=*/false, /*skip_h_scenes=*/true, repaired, nullptr);
    const std::set<int> expected_suspect_unmasked = {3, 4, 6, 7};
    EXPECT_EQ(suspect_unmasked, expected_suspect_unmasked);

    const std::set<int> suspect_include_h = compute_editable_ids(
        slice, scores, /*review_all_in_window=*/false, /*skip_h_scenes=*/false, repaired,
        &h_scene_mask);
    const std::set<int> expected_suspect_include_h = {3, 4, 6, 7};
    EXPECT_EQ(suspect_include_h, expected_suspect_include_h);

    const std::set<int> review_all_skip_h = compute_editable_ids(
        slice, scores, /*review_all_in_window=*/true, /*skip_h_scenes=*/true, repaired,
        &h_scene_mask);
    const std::set<int> expected_review_all_skip_h = {1, 2, 3, 4, 7};
    EXPECT_EQ(review_all_skip_h, expected_review_all_skip_h);
}

}  // namespace
}  // namespace spot_checker
