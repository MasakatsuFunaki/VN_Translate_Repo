// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#pragma once

#include "domain/spot_check_types.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace spot_checker {

inline constexpr double kCusumCombinedZWeight = 0.05;

// Full-novel statistical weakness hotspot detector (Lakos PIMPL facade).
// Combines `score_corpus_lines` with per-script Page's CUSUM regime-shift
// accumulation, boundary-renormalized per-script Gaussian KDE smoothing,
// monotonic-deque `cusum_peak`, `combined_z`-sorted greedy Non-Maximum
// Suppression (NMS), and adaptive neighbor contagion.
class HotspotDetector {
public:
    explicit HotspotDetector(HotspotConfig config = {});
    ~HotspotDetector();

    HotspotDetector(HotspotDetector&&) noexcept;
    HotspotDetector& operator=(HotspotDetector&&) noexcept;

    HotspotDetector(const HotspotDetector&) = delete;
    HotspotDetector& operator=(const HotspotDetector&) = delete;

    const HotspotConfig& config() const noexcept;

    OlsModel fit_length_regression(
        const std::vector<ScriptEntry>& entries) const;

    LineWeaknessScore score_single_line(const ScriptEntry& entry,
                                        const OlsModel& ols_model,
                                        bool apply_h_skip = true) const;

    // Scores `entries`, resets Page's CUSUM at each `script_name` boundary, and
    // applies Gaussian KDE truncated strictly within `[script_begin, script_end)`
    // and renormalized by active kernel weight sum (eliminating endpoint clamping
    // distortion on line 0, line N-1, and short scripts where `N < window_size`).
    std::vector<LineWeaknessScore> score_entries(
        const std::vector<ScriptEntry>& entries,
        OlsModel* out_model = nullptr) const;

    // Ranks non-overlapping candidate windows of length
    // `effective_w = min(config.window_size, segment_len)`:
    // - When `config.skip_h_scenes == true`, treats sustained H-scene spans
    //   (`detect_h_scene_spans(entries)`) as hard barriers, evaluating candidate
    //   windows strictly inside maximal non-H intervals so zero returned windows
    //   overlap a sustained H-scene span while shorter non-H scenes adjacent to
    //   an H-scene still surface their defect windows. Short non-H dirty/crude
    //   banter outside sustained H-spans is never suppressed.
    // - Never returns a window with `energy <= 0.0` or `z_score < config.z_threshold`
    //   (a 100%-clean corpus returns an empty vector, never a bogus offset-0 window).
    // - Ranks candidates monotonically by `z_score = kde_z + kCusumCombinedZWeight * cusum_peak`
    //   and enforces `|offset_a - offset_b| >= config.nms_min_separation`.
    // - Caps returned windows at `config.max_windows` when `config.max_windows > 0`.
    std::vector<HotspotWindow> select_hotspots(
        const std::vector<ScriptEntry>& entries,
        std::vector<LineWeaknessScore>* out_scores = nullptr) const;

    // Evaluates whether accepted improvements inside `[window_offset,
    // window_offset + window_len)` warrant expanding to immediate left and/or
    // right neighbor windows:
    // - Ignores improvements outside `[window_offset, window_offset + window_len)`.
    // - Expands both sides (clamped to `[0, total_entries)`) when
    //   `in_window_fixes / window_len >= config.contagion_defect_rate`, or expands
    //   the respective side when a fix lies within `config.boundary_margin` of the
    //   left or right window edge (using overflow-safe subtraction).
    NeighborExpansion evaluate_contagion(
        std::size_t window_offset,
        std::size_t window_len,
        std::size_t total_entries,
        const std::vector<Improvement>& window_improvements) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace spot_checker
