// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "detector/hotspot_detector.h"

#include "detector/weakness_signals.h"
#include "domain/vn_line_syntax.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace spot_checker {

namespace {

constexpr double kMinGaussianSigma = 1.0;
constexpr double kKernelTruncateSigmas = 3.0;
constexpr double kGaussianExponentHalf = -0.5;
constexpr double kMadToStdScale = 1.4826;
constexpr double kMinBaselineScale = 1.0;
constexpr double kVarianceEpsilon = 1e-9;
constexpr std::size_t kMedianHalfDivisor = 2;
constexpr std::size_t kMinCandidatesForStd = 2;
constexpr std::size_t kMinWindowsForAcrossZ = 3;

void apply_script_cusum_and_kde(std::vector<LineWeaknessScore>& scores,
                                std::size_t seg_begin,
                                std::size_t seg_end,
                                const HotspotConfig& cfg) {
    if (seg_begin >= seg_end) {
        return;
    }
    const std::size_t seg_len = seg_end - seg_begin;
    const double slack_k = std::max(0.0, cfg.cusum_slack_k);

    double cusum = 0.0;
    for (std::size_t i = seg_begin; i < seg_end; ++i) {
        cusum = std::max(0.0, cusum + scores[i].total_score - slack_k);
        scores[i].cusum_stat = cusum;
    }

    if (seg_len == 1U) {
        scores[seg_begin].smoothed_density = scores[seg_begin].total_score;
        return;
    }

    double eff_sigma =
        (cfg.gaussian_sigma > 0.0) ? cfg.gaussian_sigma : kMinGaussianSigma;
    if (cfg.window_size > 0U && seg_len < cfg.window_size) {
        eff_sigma = std::max(
            kMinGaussianSigma,
            eff_sigma * static_cast<double>(seg_len) /
                static_cast<double>(cfg.window_size));
    }

    const auto max_radius = static_cast<std::size_t>(
        std::ceil(kKernelTruncateSigmas * eff_sigma));
    const std::size_t radius = std::min(seg_len - 1U, max_radius);

    std::vector<double> kernel(radius + 1U, 1.0);
    for (std::size_t d = 1U; d <= radius; ++d) {
        const double u = static_cast<double>(d) / eff_sigma;
        kernel[d] = std::exp(kGaussianExponentHalf * u * u);
    }

    for (std::size_t i = seg_begin; i < seg_end; ++i) {
        const std::size_t left =
            (i - seg_begin > radius) ? (i - radius) : seg_begin;
        const std::size_t right =
            (seg_end - 1U - i > radius) ? (i + radius) : (seg_end - 1U);

        double weighted_sum = 0.0;
        double weight_norm = 0.0;
        for (std::size_t j = left; j <= right; ++j) {
            const std::size_t dist = (j >= i) ? (j - i) : (i - j);
            const double w = kernel[dist];
            weighted_sum += w * scores[j].total_score;
            weight_norm += w;
        }
        scores[i].smoothed_density =
            (weight_norm > 0.0) ? (weighted_sum / weight_norm)
                                : scores[i].total_score;
    }
}

struct BaselineStats {
    double median = 0.0;
    double scale = kMinBaselineScale;
};

BaselineStats compute_robust_line_baseline(
    const std::vector<LineWeaknessScore>& scores,
    const std::vector<bool>& h_mask) {
    BaselineStats stats;
    if (scores.empty()) {
        return stats;
    }

    std::vector<double> vals;
    vals.reserve(scores.size());
    for (std::size_t i = 0; i < scores.size(); ++i) {
        if (i < h_mask.size() && h_mask[i]) {
            continue;
        }
        vals.push_back(scores[i].smoothed_density);
    }
    if (vals.empty()) {
        return stats;
    }

    const std::size_t mid = vals.size() / kMedianHalfDivisor;
    std::nth_element(vals.begin(), vals.begin() + static_cast<std::ptrdiff_t>(mid),
                     vals.end());
    stats.median = vals[mid];

    std::vector<double> abs_devs;
    abs_devs.reserve(vals.size());
    for (double v : vals) {
        abs_devs.push_back(std::abs(v - stats.median));
    }
    std::nth_element(abs_devs.begin(),
                     abs_devs.begin() + static_cast<std::ptrdiff_t>(mid),
                     abs_devs.end());
    stats.scale = std::max(kMinBaselineScale, abs_devs[mid] * kMadToStdScale);
    return stats;
}

std::vector<std::pair<std::size_t, std::size_t>> collect_non_masked_intervals(
    const std::vector<ScriptEntry>& entries,
    const std::vector<bool>& h_mask) {
    std::vector<std::pair<std::size_t, std::size_t>> intervals;
    const std::size_t total_len = entries.size();
    if (total_len == 0U) {
        return intervals;
    }

    std::size_t i = 0;
    while (i < total_len) {
        while (i < total_len && i < h_mask.size() && h_mask[i]) {
            ++i;
        }
        if (i >= total_len) {
            break;
        }
        const std::size_t iv_begin = i;
        const std::string_view seg_script = entries[iv_begin].script_name;
        while (i < total_len && entries[i].script_name == seg_script &&
               !(i < h_mask.size() && h_mask[i])) {
            ++i;
        }
        intervals.emplace_back(iv_begin, i);
    }
    return intervals;
}

std::vector<double> sliding_window_max_slice(const std::vector<double>& values,
                                             std::size_t slice_begin,
                                             std::size_t slice_end,
                                             std::size_t window_len) {
    const std::size_t n =
        (slice_end > slice_begin) ? (slice_end - slice_begin) : 0U;
    if (n == 0U || window_len == 0U || window_len > n) {
        return {};
    }
    const std::size_t out_len = n - window_len + 1U;
    std::vector<double> result(out_len, 0.0);
    std::deque<std::size_t> dq;

    for (std::size_t i = 0; i < n; ++i) {
        while (!dq.empty() && dq.front() + window_len <= i) {
            dq.pop_front();
        }
        while (!dq.empty() &&
               values[slice_begin + dq.back()] <= values[slice_begin + i]) {
            dq.pop_back();
        }
        dq.push_back(i);
        if (i + 1U >= window_len) {
            result[i + 1U - window_len] = values[slice_begin + dq.front()];
        }
    }
    return result;
}

std::vector<std::size_t> sliding_window_peak_index_slice(
    const std::vector<LineWeaknessScore>& scores,
    std::size_t slice_begin,
    std::size_t slice_end,
    std::size_t window_len) {
    const std::size_t n =
        (slice_end > slice_begin) ? (slice_end - slice_begin) : 0U;
    if (n == 0U || window_len == 0U || window_len > n) {
        return {};
    }
    const std::size_t out_len = n - window_len + 1U;
    std::vector<std::size_t> result(out_len, 0U);
    std::deque<std::size_t> dq;

    auto is_better_or_equal = [&](std::size_t lhs, std::size_t rhs) noexcept {
        const LineWeaknessScore& a = scores[slice_begin + lhs];
        const LineWeaknessScore& b = scores[slice_begin + rhs];
        if (a.total_score != b.total_score) {
            return a.total_score < b.total_score;
        }
        return a.smoothed_density <= b.smoothed_density;
    };

    for (std::size_t i = 0; i < n; ++i) {
        while (!dq.empty() && dq.front() + window_len <= i) {
            dq.pop_front();
        }
        while (!dq.empty() && is_better_or_equal(dq.back(), i)) {
            dq.pop_back();
        }
        dq.push_back(i);
        if (i + 1U >= window_len) {
            result[i + 1U - window_len] = slice_begin + dq.front();
        }
    }
    return result;
}

}  // namespace

struct HotspotDetector::Impl {
    HotspotConfig config;
};

HotspotDetector::HotspotDetector(HotspotConfig config)
    : impl_(std::make_unique<Impl>(Impl{std::move(config)})) {}

HotspotDetector::~HotspotDetector() = default;
HotspotDetector::HotspotDetector(HotspotDetector&&) noexcept = default;
HotspotDetector& HotspotDetector::operator=(HotspotDetector&&) noexcept = default;

const HotspotConfig& HotspotDetector::config() const noexcept {
    return impl_->config;
}

OlsModel HotspotDetector::fit_length_regression(
    const std::vector<ScriptEntry>& entries) const {
    return spot_checker::fit_length_regression(entries, impl_->config.skip_h_scenes);
}

LineWeaknessScore HotspotDetector::score_single_line(
    const ScriptEntry& entry,
    const OlsModel& ols_model,
    bool apply_h_skip) const {
    return spot_checker::score_single_line(
        entry, ols_model, apply_h_skip, impl_->config.fix_punctuation);
}

std::vector<LineWeaknessScore> HotspotDetector::score_entries(
    const std::vector<ScriptEntry>& entries,
    OlsModel* out_model) const {
    const OlsModel model = fit_length_regression(entries);
    if (out_model != nullptr) {
        *out_model = model;
    }
    if (entries.empty()) {
        return {};
    }

    std::vector<LineWeaknessScore> scores =
        score_corpus_lines(entries, model, impl_->config);

    const std::vector<bool> h_mask =
        impl_->config.skip_h_scenes ? compute_h_scene_line_mask(entries)
                                    : std::vector<bool>{};

    std::size_t seg_begin = 0;
    while (seg_begin < entries.size()) {
        std::size_t seg_end = seg_begin + 1U;
        while (seg_end < entries.size() &&
               entries[seg_end].script_name == entries[seg_begin].script_name) {
            ++seg_end;
        }
        if (!impl_->config.skip_h_scenes) {
            apply_script_cusum_and_kde(scores, seg_begin, seg_end, impl_->config);
        } else {
            std::size_t sub_i = seg_begin;
            while (sub_i < seg_end) {
                while (sub_i < seg_end && sub_i < h_mask.size() && h_mask[sub_i]) {
                    ++sub_i;
                }
                if (sub_i >= seg_end) {
                    break;
                }
                const std::size_t sub_begin = sub_i;
                while (sub_i < seg_end &&
                       !(sub_i < h_mask.size() && h_mask[sub_i])) {
                    ++sub_i;
                }
                apply_script_cusum_and_kde(
                    scores, sub_begin, sub_i, impl_->config);
            }
        }
        seg_begin = seg_end;
    }

    return scores;
}

std::vector<HotspotWindow> HotspotDetector::select_hotspots(
    const std::vector<ScriptEntry>& entries,
    std::vector<LineWeaknessScore>* out_scores) const {
    std::vector<LineWeaknessScore> scores = score_entries(entries, nullptr);
    if (entries.empty()) {
        if (out_scores != nullptr) {
            out_scores->clear();
        }
        return {};
    }

    const std::vector<bool> h_mask =
        impl_->config.skip_h_scenes ? compute_h_scene_line_mask(entries)
                                    : std::vector<bool>{};
    const std::vector<std::pair<std::size_t, std::size_t>> intervals =
        collect_non_masked_intervals(entries, h_mask);
    if (intervals.empty()) {
        if (out_scores != nullptr) {
            *out_scores = std::move(scores);
        }
        return {};
    }

    const std::size_t configured_w =
        (impl_->config.window_size > 0U) ? impl_->config.window_size : 1U;

    std::vector<double> prefix_raw(entries.size() + 1U, 0.0);
    std::vector<double> prefix_kde(entries.size() + 1U, 0.0);
    std::vector<double> cusum_vals(entries.size(), 0.0);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        prefix_raw[i + 1U] = prefix_raw[i] + scores[i].total_score;
        prefix_kde[i + 1U] = prefix_kde[i] + scores[i].smoothed_density;
        cusum_vals[i] = scores[i].cusum_stat;
    }

    std::size_t total_non_h_lines = 0;
    std::size_t total_candidates = 0;
    double mean_kde_per_line = 0.0;
    double m2_kde_per_line = 0.0;

    for (const auto& [iv_begin, iv_end] : intervals) {
        const std::size_t iv_len = iv_end - iv_begin;
        total_non_h_lines += iv_len;
        const std::size_t eff_w = std::min(configured_w, iv_len);
        const std::size_t iv_cands = iv_len - eff_w + 1U;
        for (std::size_t local_off = 0; local_off < iv_cands; ++local_off) {
            const std::size_t off = iv_begin + local_off;
            const double kde_e = prefix_kde[off + eff_w] - prefix_kde[off];
            const double kde_per_line = kde_e / static_cast<double>(eff_w);

            ++total_candidates;
            const auto k = static_cast<double>(total_candidates);
            const double delta = kde_per_line - mean_kde_per_line;
            mean_kde_per_line += delta / k;
            m2_kde_per_line += delta * (kde_per_line - mean_kde_per_line);
        }
    }

    const bool has_across_window_std =
        total_candidates >= kMinCandidatesForStd &&
        (m2_kde_per_line / static_cast<double>(total_candidates)) >
            kVarianceEpsilon;
    const double std_kde_per_line =
        has_across_window_std
            ? std::sqrt(m2_kde_per_line / static_cast<double>(total_candidates))
            : 0.0;

    const BaselineStats line_base = compute_robust_line_baseline(scores, h_mask);

    std::vector<HotspotWindow> candidates;
    candidates.reserve(total_candidates);
    for (const auto& [iv_begin, iv_end] : intervals) {
        const std::size_t iv_len = iv_end - iv_begin;
        const std::size_t eff_w = std::min(configured_w, iv_len);
        const std::size_t iv_cands = iv_len - eff_w + 1U;

        const double null_win_mean =
            static_cast<double>(eff_w) * line_base.median;
        const double null_win_std =
            std::sqrt(static_cast<double>(eff_w)) * line_base.scale;

        const std::vector<double> cusum_peaks =
            sliding_window_max_slice(cusum_vals, iv_begin, iv_end, eff_w);
        const std::vector<std::size_t> peak_indices =
            sliding_window_peak_index_slice(scores, iv_begin, iv_end, eff_w);

        for (std::size_t local_off = 0; local_off < iv_cands; ++local_off) {
            const std::size_t off = iv_begin + local_off;
            const double raw_e = prefix_raw[off + eff_w] - prefix_raw[off];
            const double kde_e = prefix_kde[off + eff_w] - prefix_kde[off];
            if (raw_e <= 0.0 || kde_e <= 0.0) {
                continue;
            }
            const double intrinsic_z =
                (kde_e - null_win_mean) / null_win_std;
            double kde_z = intrinsic_z;
            if (has_across_window_std) {
                const double kde_per_line = kde_e / static_cast<double>(eff_w);
                const double across_z =
                    (kde_per_line - mean_kde_per_line) / std_kde_per_line;
                kde_z = (total_non_h_lines >= kMinWindowsForAcrossZ * eff_w)
                            ? std::max(across_z, intrinsic_z)
                            : intrinsic_z;
            }
            const double combined_z =
                kde_z + kCusumCombinedZWeight * cusum_peaks[local_off];
            if (!std::isfinite(combined_z) ||
                combined_z < impl_->config.z_threshold) {
                continue;
            }

            const std::size_t peak_idx = peak_indices[local_off];
            HotspotWindow win;
            win.offset = off;
            win.length = eff_w;
            win.peak_index = peak_idx;
            win.energy = kde_e;
            win.cusum_peak = cusum_peaks[local_off];
            win.z_score = combined_z;
            win.peak_line_score = scores[peak_idx].total_score;
            win.dominant_signal = scores[peak_idx].dominant_signal;
            candidates.push_back(std::move(win));
        }
    }

    std::sort(candidates.begin(),
              candidates.end(),
              [](const HotspotWindow& a, const HotspotWindow& b) noexcept {
                  if (a.z_score != b.z_score) {
                      return a.z_score > b.z_score;
                  }
                  if (a.energy != b.energy) {
                      return a.energy > b.energy;
                  }
                  return a.offset < b.offset;
              });

    std::vector<HotspotWindow> selected;
    for (HotspotWindow& cand : candidates) {
        if (impl_->config.max_windows > 0U &&
            selected.size() >= impl_->config.max_windows) {
            break;
        }
        bool suppressed = false;
        for (const HotspotWindow& kept : selected) {
            const std::size_t dist = (cand.offset >= kept.offset)
                                         ? (cand.offset - kept.offset)
                                         : (kept.offset - cand.offset);
            if (dist < impl_->config.nms_min_separation) {
                suppressed = true;
                break;
            }
        }
        if (!suppressed) {
            selected.push_back(std::move(cand));
        }
    }

    if (out_scores != nullptr) {
        *out_scores = std::move(scores);
    }
    return selected;
}

NeighborExpansion HotspotDetector::evaluate_contagion(
    std::size_t window_offset,
    std::size_t window_len,
    std::size_t total_entries,
    const std::vector<Improvement>& window_improvements) const {
    NeighborExpansion exp;
    if (window_len == 0U || total_entries == 0U ||
        window_offset >= total_entries || window_improvements.empty()) {
        return exp;
    }

    const std::size_t window_end =
        window_offset + std::min(window_len, total_entries - window_offset);
    const std::size_t actual_len = window_end - window_offset;
    if (actual_len == 0U) {
        return exp;
    }

    std::unordered_set<std::size_t> seen_in_window;
    bool near_left = false;
    bool near_right = false;
    for (const Improvement& imp : window_improvements) {
        if (imp.global_index < window_offset || imp.global_index >= window_end) {
            continue;
        }
        seen_in_window.insert(imp.global_index);
        if ((imp.global_index - window_offset) < impl_->config.boundary_margin) {
            near_left = true;
        }
        if ((window_end - 1U - imp.global_index) < impl_->config.boundary_margin) {
            near_right = true;
        }
    }

    if (seen_in_window.empty()) {
        return exp;
    }

    const double defect_rate = static_cast<double>(seen_in_window.size()) /
                               static_cast<double>(actual_len);
    const bool high_rate = defect_rate >= impl_->config.contagion_defect_rate;
    const bool want_left = high_rate || near_left;
    const bool want_right = high_rate || near_right;

    const std::size_t step_w =
        (impl_->config.window_size > 0U) ? impl_->config.window_size : actual_len;

    if (want_left && window_offset > 0U) {
        exp.expand_left = true;
        exp.left_offset =
            (window_offset > step_w) ? (window_offset - step_w) : 0U;
        exp.left_length = window_offset - exp.left_offset;
    }
    if (want_right && window_end < total_entries) {
        exp.expand_right = true;
        exp.right_offset = window_end;
        exp.right_length = std::min(step_w, total_entries - window_end);
    }

    return exp;
}

}  // namespace spot_checker
