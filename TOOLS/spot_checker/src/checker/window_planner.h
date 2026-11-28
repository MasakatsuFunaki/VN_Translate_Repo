// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once

#include "domain/spot_check_types.h"

#include <cstddef>
#include <set>
#include <utility>
#include <vector>

namespace spot_checker {

inline constexpr double kEditableSuspectScoreThreshold = 1.8;

// Disjoint sorted half-open interval set `[begin, end)` tracking inspected
// corpus spans so adaptive neighbor contagion and checkpoint resumes never
// re-send already-checked lines on partially overlapping windows.
class IntervalTracker {
public:
    IntervalTracker() = default;

    // Inserts `[offset, offset + length)` (no-op if `length == 0`), coalescing
    // overlapping or adjacent intervals.
    void mark_visited(std::size_t offset, std::size_t length);

    // Returns true iff `length == 0` or every index in `[offset, offset + length)`
    // is already covered.
    bool is_fully_covered(std::size_t offset, std::size_t length) const noexcept;

    // Trims covered prefix/suffix indices from `[offset, offset + length)` and
    // returns the minimal bounding `{trimmed_offset, trimmed_length}` of all
    // uncovered indices inside `[offset, offset + length)` (returns `{offset, 0}`
    // when fully covered).
    std::pair<std::size_t, std::size_t> trim_uncovered_span(
        std::size_t offset,
        std::size_t length) const noexcept;

    const std::vector<std::pair<std::size_t, std::size_t>>& intervals() const noexcept {
        return intervals_;
    }

    void clear() noexcept { intervals_.clear(); }

private:
    std::vector<std::pair<std::size_t, std::size_t>> intervals_;
};

// Extracts a target window `[offset, min(entries.size(), offset + window_size))`
// into `slice.target`, with up to `context_before` preceding entries in
// `slice.context_before` and up to `context_after` following entries in
// `slice.context_after` (`slice.target_start = offset`).
// - When `h_scene_mask != nullptr` (size `entries.size()`), clips
//   `context_before` and `context_after` at the nearest sustained H-scene
//   boundary (`(*h_scene_mask)[idx] == true`) so surrounding context never
//   crosses into a sustained H-scene span.
// - Returns an empty `WindowSlice` if `offset >= entries.size()` or
//   `window_size == 0`.
WindowSlice build_window_slice(const std::vector<ScriptEntry>& entries,
                               std::size_t offset,
                               std::size_t window_size,
                               std::size_t context_before,
                               std::size_t context_after,
                               const std::vector<bool>* h_scene_mask = nullptr);

// Computes the 1-based `editable_ids` (`1..slice.target.size()`) unlocked for
// editing inside `slice.target`:
// - Rows whose `global_index` is in `repaired_indices`, passthrough/silence/
//   non-JP rows, and (when `skip_h_scenes == true` and `h_scene_mask != nullptr`)
//   rows inside sustained H-scene spans (`global_index < h_scene_mask->size() &&
//   (*h_scene_mask)[global_index]`) are always locked. Non-H dirty/crude banter
//   outside sustained H-scene spans (`h_scene_mask == nullptr` or
//   `(*h_scene_mask)[global_index] == false`) is never locked based on
//   single-line explicit words.
// - When `review_all_in_window == true`, all remaining target rows `1..N` are unlocked.
// - When `review_all_in_window == false`, unlocks target rows whose
//   `novel_scores[global_index].total_score >= kEditableSuspectScoreThreshold`
//   plus their immediate `+/-1` banter partners inside `slice.target` (provided
//   the partner itself is not locked by the rules above).
std::set<int> compute_editable_ids(
    const WindowSlice& slice,
    const std::vector<LineWeaknessScore>& novel_scores,
    bool review_all_in_window = true,
    bool skip_h_scenes = true,
    const std::set<std::size_t>& repaired_indices = {},
    const std::vector<bool>* h_scene_mask = nullptr);

}  // namespace spot_checker
