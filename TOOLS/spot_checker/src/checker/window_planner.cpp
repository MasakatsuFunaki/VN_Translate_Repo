// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "checker/window_planner.h"

#include "common/utf8_text.h"
#include "domain/vn_line_syntax.h"

#include <algorithm>
#include <limits>
#include <string>

namespace spot_checker {
namespace {

constexpr std::size_t saturated_add(std::size_t a, std::size_t b) noexcept {
    constexpr std::size_t kMaxSize = std::numeric_limits<std::size_t>::max();
    if (b > kMaxSize - a) {
        return kMaxSize;
    }
    return a + b;
}

bool is_locked_target_entry(const ScriptEntry& entry,
                            bool skip_h_scenes,
                            const std::set<std::size_t>& repaired_indices,
                            const std::vector<bool>* h_scene_mask) {
    if (repaired_indices.count(entry.global_index) != 0) {
        return true;
    }
    if (is_engine_asset_or_command_token(entry.jp_text, entry.current_en)) {
        return true;
    }

    const std::string jp_body = strip_speaker_prefix_body(entry.jp_text);
    if (!has_real_japanese(jp_body) || is_pure_silence_or_punctuation(jp_body)) {
        return true;
    }

    const std::string en_body = strip_speaker_prefix_body(entry.current_en);
    if (trim_ascii_view(en_body) == trim_ascii_view(jp_body) ||
        trim_ascii_view(entry.current_en) == trim_ascii_view(entry.jp_text)) {
        return true;
    }

    if (skip_h_scenes && h_scene_mask != nullptr &&
        entry.global_index < h_scene_mask->size() &&
        (*h_scene_mask)[entry.global_index]) {
        return true;
    }
    return false;
}

}  // namespace

void IntervalTracker::mark_visited(std::size_t offset, std::size_t length) {
    if (length == 0) {
        return;
    }
    const std::size_t begin = offset;
    const std::size_t end = saturated_add(offset, length);
    if (end <= begin) {
        return;
    }

    auto it = std::lower_bound(
        intervals_.begin(),
        intervals_.end(),
        begin,
        [](const std::pair<std::size_t, std::size_t>& iv, std::size_t val) {
            return iv.second < val;
        });

    if (it == intervals_.end() || it->first > end) {
        intervals_.insert(it, {begin, end});
        return;
    }

    std::size_t merged_begin = std::min(begin, it->first);
    std::size_t merged_end = std::max(end, it->second);
    auto erase_end = it + 1;
    while (erase_end != intervals_.end() && erase_end->first <= merged_end) {
        merged_end = std::max(merged_end, erase_end->second);
        ++erase_end;
    }
    *it = {merged_begin, merged_end};
    intervals_.erase(it + 1, erase_end);
}

bool IntervalTracker::is_fully_covered(std::size_t offset,
                                       std::size_t length) const noexcept {
    if (length == 0) {
        return true;
    }
    const std::size_t end = saturated_add(offset, length);
    if (end <= offset) {
        return true;
    }

    const auto it = std::lower_bound(
        intervals_.begin(),
        intervals_.end(),
        offset,
        [](const std::pair<std::size_t, std::size_t>& iv, std::size_t val) {
            return iv.second <= val;
        });
    return it != intervals_.end() && it->first <= offset && end <= it->second;
}

std::pair<std::size_t, std::size_t> IntervalTracker::trim_uncovered_span(
    std::size_t offset,
    std::size_t length) const noexcept {
    if (length == 0) {
        return {offset, 0};
    }
    std::size_t cur_begin = offset;
    std::size_t cur_end = saturated_add(offset, length);
    if (cur_end <= cur_begin) {
        return {offset, 0};
    }

    const auto head_it = std::lower_bound(
        intervals_.begin(),
        intervals_.end(),
        cur_begin,
        [](const std::pair<std::size_t, std::size_t>& iv, std::size_t val) {
            return iv.second <= val;
        });
    if (head_it != intervals_.end() && head_it->first <= cur_begin) {
        if (head_it->second >= cur_end) {
            return {offset, 0};
        }
        cur_begin = head_it->second;
    }

    const std::size_t last_index = cur_end - 1;
    const auto tail_it = std::lower_bound(
        intervals_.begin(),
        intervals_.end(),
        last_index,
        [](const std::pair<std::size_t, std::size_t>& iv, std::size_t val) {
            return iv.second <= val;
        });
    if (tail_it != intervals_.end() &&
        tail_it->first <= last_index &&
        cur_end <= tail_it->second) {
        cur_end = tail_it->first;
    }

    if (cur_end <= cur_begin) {
        return {offset, 0};
    }
    return {cur_begin, cur_end - cur_begin};
}

WindowSlice build_window_slice(const std::vector<ScriptEntry>& entries,
                               std::size_t offset,
                               std::size_t window_size,
                               std::size_t context_before,
                               std::size_t context_after,
                               const std::vector<bool>* h_scene_mask) {
    if (entries.empty() || offset >= entries.size() || window_size == 0) {
        return {};
    }

    WindowSlice slice;
    slice.target_start = offset;

    const std::size_t remaining_from_offset = entries.size() - offset;
    const std::size_t target_len = std::min(window_size, remaining_from_offset);
    const std::size_t target_end = offset + target_len;

    const std::size_t min_before_start =
        (offset > context_before) ? (offset - context_before) : 0U;
    std::size_t before_start = min_before_start;
    if (h_scene_mask != nullptr) {
        before_start = offset;
        while (before_start > min_before_start) {
            const std::size_t prev_idx = before_start - 1U;
            if (prev_idx < h_scene_mask->size() && (*h_scene_mask)[prev_idx]) {
                break;
            }
            before_start = prev_idx;
        }
    }

    const std::size_t remaining_after_target = entries.size() - target_end;
    const std::size_t max_after_end =
        target_end + std::min(context_after, remaining_after_target);
    std::size_t after_end = max_after_end;
    if (h_scene_mask != nullptr) {
        after_end = target_end;
        while (after_end < max_after_end) {
            if (after_end < h_scene_mask->size() && (*h_scene_mask)[after_end]) {
                break;
            }
            ++after_end;
        }
    }

    slice.context_before.assign(
        entries.begin() + static_cast<std::ptrdiff_t>(before_start),
        entries.begin() + static_cast<std::ptrdiff_t>(offset));
    slice.target.assign(
        entries.begin() + static_cast<std::ptrdiff_t>(offset),
        entries.begin() + static_cast<std::ptrdiff_t>(target_end));
    slice.context_after.assign(
        entries.begin() + static_cast<std::ptrdiff_t>(target_end),
        entries.begin() + static_cast<std::ptrdiff_t>(after_end));
    return slice;
}

std::set<int> compute_editable_ids(
    const WindowSlice& slice,
    const std::vector<LineWeaknessScore>& novel_scores,
    bool review_all_in_window,
    bool skip_h_scenes,
    const std::set<std::size_t>& repaired_indices,
    const std::vector<bool>* h_scene_mask) {
    std::set<int> editable;
    if (slice.target.empty()) {
        return editable;
    }

    const std::size_t count = slice.target.size();
    std::vector<bool> locked(count, false);
    for (std::size_t i = 0; i < count; ++i) {
        locked[i] = is_locked_target_entry(
            slice.target[i], skip_h_scenes, repaired_indices, h_scene_mask);
    }

    if (review_all_in_window) {
        for (std::size_t i = 0; i < count; ++i) {
            if (!locked[i]) {
                editable.insert(static_cast<int>(i + 1));
            }
        }
        return editable;
    }

    for (std::size_t i = 0; i < count; ++i) {
        if (locked[i]) {
            continue;
        }
        const std::size_t g_idx = slice.target[i].global_index;
        if (g_idx >= novel_scores.size()) {
            continue;
        }
        if (novel_scores[g_idx].total_score < kEditableSuspectScoreThreshold) {
            continue;
        }
        if (i > 0 && !locked[i - 1]) {
            editable.insert(static_cast<int>(i));
        }
        editable.insert(static_cast<int>(i + 1));
        if (i + 1 < count && !locked[i + 1]) {
            editable.insert(static_cast<int>(i + 2));
        }
    }
    return editable;
}

}  // namespace spot_checker
