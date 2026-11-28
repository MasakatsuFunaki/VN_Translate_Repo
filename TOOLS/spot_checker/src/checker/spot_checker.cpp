// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "checker/spot_checker.h"

#include "checker/fix_validator.h"
#include "checker/prompt_codec.h"
#include "checker/spot_check_checkpoint.h"
#include "checker/window_planner.h"
#include "client/argon_client.h"
#include "common/utf8_text.h"
#include "common/win32_cli_env.h"
#include "detector/hotspot_detector.h"
#include "domain/tone_glossary.h"
#include "domain/vn_line_syntax.h"
#include "ports/clock_and_logger.h"
#include "ports/crypt_sync.h"
#include "ports/file_store.h"
#include "ports/llm_transport.h"
#include "storage/script_repository.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace spot_checker {
namespace {

namespace fs = std::filesystem;

constexpr double kMinValidConfidenceExclusive = 0.0;
constexpr double kMaxValidConfidenceInclusive = 1.0;
constexpr double kMqmSortTieEpsilon = 1e-9;
constexpr double kPercentMultiplier = 100.0;
constexpr std::size_t kMinScaledNmsSeparation = 1;
constexpr std::size_t kMinScaledBoundaryMargin = 1;
constexpr double kMinScaledGaussianSigma = 1.0;
constexpr std::size_t kBisectHalfDivisor = 2;
constexpr std::size_t kProgressBufferBytes = 256;
constexpr std::string_view kScriptOutputDir = "script_output";
constexpr std::string_view kDefaultCheckpointFilename = "argon_spot_check.json";
constexpr std::string_view kHotspotModeName = "hotspot";
constexpr std::string_view kLinearModeName = "linear";
constexpr std::string_view kDefaultBlockReason = "CONTENT_BLOCKED";

struct WorkItem {
    std::size_t offset = 0;
    std::size_t length = 0;
    bool force_no_context = false;
};

constexpr std::size_t saturated_add(std::size_t a, std::size_t b) noexcept {
    constexpr std::size_t kMaxSize = std::numeric_limits<std::size_t>::max();
    if (b > kMaxSize - a) {
        return kMaxSize;
    }
    return a + b;
}

std::vector<std::pair<std::size_t, std::size_t>> extract_uncovered_spans(
    const IntervalTracker& tracker,
    std::size_t offset,
    std::size_t length) {
    std::vector<std::pair<std::size_t, std::size_t>> uncovered;
    if (length == 0) {
        return uncovered;
    }
    const std::size_t span_end = saturated_add(offset, length);
    if (span_end <= offset) {
        return uncovered;
    }

    const auto& ivs = tracker.intervals();
    auto it = std::lower_bound(
        ivs.begin(),
        ivs.end(),
        offset,
        [](const std::pair<std::size_t, std::size_t>& iv, std::size_t val) {
            return iv.second <= val;
        });

    std::size_t cur = offset;
    while (cur < span_end && it != ivs.end() && it->first < span_end) {
        if (it->first > cur) {
            uncovered.emplace_back(cur, it->first - cur);
        }
        if (it->second >= span_end) {
            cur = span_end;
            break;
        }
        cur = std::max(cur, it->second);
        ++it;
    }
    if (cur < span_end) {
        uncovered.emplace_back(cur, span_end - cur);
    }
    return uncovered;
}

class BlockedSpanBookkeeper {
public:
    void clear_span(std::size_t offset, std::size_t length) {
        if (length == 0 || spans_.empty()) {
            return;
        }
        const std::size_t clear_begin = offset;
        const std::size_t clear_end = saturated_add(offset, length);
        if (clear_end <= clear_begin) {
            return;
        }

        std::vector<std::pair<std::size_t, std::size_t>> updated;
        updated.reserve(spans_.size() + 1U);
        for (const auto& [span_off, span_len] : spans_) {
            const std::size_t span_begin = span_off;
            const std::size_t span_end = saturated_add(span_off, span_len);
            if (span_end <= span_begin) {
                continue;
            }
            if (span_end <= clear_begin || span_begin >= clear_end) {
                updated.emplace_back(span_begin, span_end - span_begin);
                continue;
            }
            if (span_begin < clear_begin) {
                updated.emplace_back(span_begin, clear_begin - span_begin);
            }
            if (clear_end < span_end) {
                updated.emplace_back(clear_end, span_end - clear_end);
            }
        }
        spans_ = std::move(updated);
    }

    void record_blocked_leaf(std::size_t offset, std::size_t length) {
        if (length == 0) {
            return;
        }
        clear_span(offset, length);
        const auto pos = std::lower_bound(
            spans_.begin(),
            spans_.end(),
            offset,
            [](const std::pair<std::size_t, std::size_t>& item,
               std::size_t target_off) { return item.first < target_off; });
        spans_.insert(pos, {offset, length});
    }

    const std::vector<std::pair<std::size_t, std::size_t>>& spans()
        const noexcept {
        return spans_;
    }

    void sync_to_report(SpotCheckReport& rep) const {
        rep.blocked_spans = spans_;
        rep.blocked_offsets.clear();
        rep.blocked_offsets.reserve(spans_.size());
        std::size_t total_blocked_lines = 0;
        for (const auto& [off, len] : spans_) {
            rep.blocked_offsets.push_back(off);
            total_blocked_lines += len;
        }
        rep.windows_blocked = spans_.size();
        rep.lines_blocked = total_blocked_lines;
    }

private:
    std::vector<std::pair<std::size_t, std::size_t>> spans_;
};

fs::path path_from_utf8(std::string_view utf8_path) {
    return fs::path(utf8_to_wide(utf8_path));
}

std::string path_to_utf8(const fs::path& path) {
    return wide_to_utf8(path.wstring());
}

std::string mode_to_string(SpotCheckMode mode) {
    return (mode == SpotCheckMode::Hotspot) ? std::string(kHotspotModeName)
                                            : std::string(kLinearModeName);
}

HotspotConfig make_hotspot_config(const SpotCheckConfig& cfg) {
    HotspotConfig hcfg;
    hcfg.window_size = cfg.window_size;
    if (cfg.window_size < kDefaultWindowSize) {
        hcfg.nms_min_separation = std::max<std::size_t>(
            kMinScaledNmsSeparation,
            (cfg.window_size * kDefaultNmsMinSeparation) / kDefaultWindowSize);
        hcfg.boundary_margin = std::max<std::size_t>(
            kMinScaledBoundaryMargin,
            (cfg.window_size * kDefaultBoundaryMargin) / kDefaultWindowSize);
        hcfg.gaussian_sigma = std::max(
            kMinScaledGaussianSigma,
            kDefaultGaussianSigma * static_cast<double>(cfg.window_size) /
                static_cast<double>(kDefaultWindowSize));
    }
    hcfg.z_threshold = cfg.z_threshold;
    hcfg.skip_h_scenes = cfg.skip_h_scenes;
    hcfg.fix_punctuation = cfg.fix_punctuation;
    return hcfg;
}

bool is_locked_for_radar_unlock(const ScriptEntry& entry,
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

std::pair<std::size_t, std::size_t> clip_left_expansion_at_h_barrier(
    std::size_t left_offset,
    std::size_t left_length,
    const std::vector<bool>* h_scene_mask) noexcept {
    if (left_length == 0 || h_scene_mask == nullptr) {
        return {left_offset, left_length};
    }
    const std::size_t end = saturated_add(left_offset, left_length);
    std::size_t clipped_begin = end;
    while (clipped_begin > left_offset) {
        const std::size_t prev_idx = clipped_begin - 1U;
        if (prev_idx < h_scene_mask->size() && (*h_scene_mask)[prev_idx]) {
            break;
        }
        clipped_begin = prev_idx;
    }
    return {clipped_begin, end - clipped_begin};
}

std::pair<std::size_t, std::size_t> clip_right_expansion_at_h_barrier(
    std::size_t right_offset,
    std::size_t right_length,
    const std::vector<bool>* h_scene_mask) noexcept {
    if (right_length == 0 || h_scene_mask == nullptr) {
        return {right_offset, right_length};
    }
    const std::size_t max_end = saturated_add(right_offset, right_length);
    std::size_t clipped_end = right_offset;
    while (clipped_end < max_end) {
        if (clipped_end < h_scene_mask->size() && (*h_scene_mask)[clipped_end]) {
            break;
        }
        ++clipped_end;
    }
    return {right_offset, clipped_end - right_offset};
}

void sync_report_collections(
    const std::set<std::size_t>& visited_offsets,
    const IntervalTracker& checked_tracker,
    const BlockedSpanBookkeeper& blocked_bookkeeper,
    const std::map<std::size_t, Improvement>& merged,
    SpotCheckReport& rep) {
    rep.checked_offsets.assign(visited_offsets.begin(), visited_offsets.end());

    rep.checked_spans.clear();
    rep.checked_spans.reserve(checked_tracker.intervals().size());
    for (const auto& [begin, end] : checked_tracker.intervals()) {
        if (end > begin) {
            rep.checked_spans.emplace_back(begin, end - begin);
        }
    }

    blocked_bookkeeper.sync_to_report(rep);

    rep.improvements.clear();
    rep.improvements.reserve(merged.size());
    for (const auto& [idx, imp] : merged) {
        rep.improvements.push_back(imp);
    }
    rep.improvements_found = rep.improvements.size();
}

CheckpointState build_checkpoint_state(
    const SpotCheckConfig& cfg,
    const SpotCheckReport& rep,
    const std::vector<RadarPassageResult>& radar_results) {
    CheckpointState state;
    state.game_dir = cfg.game_dir;
    state.model = cfg.model;
    state.mode = mode_to_string(cfg.mode);
    state.total_entries = rep.total_entries;
    state.ols_slope = rep.ols_slope;
    state.ols_intercept = rep.ols_intercept;
    state.ols_sigma = rep.ols_sigma;
    state.ols_valid = rep.ols_valid;
    state.hotspots_detected = rep.hotspots_detected;
    state.h_windows_skipped = rep.h_windows_skipped;
    state.radar_calls_made = rep.radar_calls_made;
    state.radar_passages_triaged = rep.radar_passages_triaged;
    state.adaptive_expansions = rep.adaptive_expansions;
    state.windows_checked = rep.windows_checked;
    state.lines_checked = rep.lines_checked;
    state.windows_blocked = rep.windows_blocked;
    state.lines_blocked = rep.lines_blocked;
    state.editable_lines_unlocked = rep.editable_lines_unlocked;
    state.candidates_rejected_by_validator =
        rep.candidates_rejected_by_validator;
    state.script_entries_updated = rep.script_entries_updated;
    state.cache_entries_updated = rep.cache_entries_updated;
    state.token_usage = rep.token_usage;
    state.checked_offsets = rep.checked_offsets;
    state.checked_spans = rep.checked_spans;
    state.blocked_offsets = rep.blocked_offsets;
    state.blocked_spans = rep.blocked_spans;
    state.radar_results = radar_results;
    state.ranked_hotspots = rep.ranked_hotspots;
    state.improvements = rep.improvements;
    return state;
}

void record_llm_usage(const LlmResponse& resp,
                      bool is_radar,
                      std::size_t& run_api_calls,
                      TokenUsageSummary& usage) noexcept {
    ++run_api_calls;
    ++usage.api_calls;
    if (is_radar) {
        ++usage.radar_calls_made;
    } else {
        ++usage.repair_calls_made;
    }
    usage.prompt_tokens += resp.prompt_tokens;
    usage.output_tokens += resp.output_tokens;
    usage.thought_tokens += resp.thought_tokens;
}

}  // namespace

struct SpotChecker::Impl {
    SpotCheckConfig config;
    std::string output_json_path;
    ToneGlossary glossary;
    std::string system_prompt;
    std::vector<ScriptEntry> entries;

    std::unique_ptr<IClock> owned_clock;
    std::unique_ptr<ILogSink> owned_log_sink;
    std::unique_ptr<IFileStore> owned_file_store;
    std::unique_ptr<ICryptSync> owned_crypt_sync;
    std::unique_ptr<ILlmTransport> owned_transport;

    IClock* clock = nullptr;
    ILogSink* log_sink = nullptr;
    IFileStore* file_store = nullptr;
    ICryptSync* crypt_sync = nullptr;
    ILlmTransport* transport = nullptr;

    ILlmTransport& ensure_transport() {
        if (transport != nullptr) {
            return *transport;
        }
        ArgonClientOptions opts;
        opts.model = config.model;
        opts.clock = clock;
        opts.log_sink = log_sink;
        owned_transport = std::make_unique<ArgonClient>(std::move(opts));
        transport = owned_transport.get();
        return *transport;
    }

    Result<LlmResponse> invoke_llm(std::string user_prompt) {
        LlmRequest req;
        req.system_prompt = system_prompt;
        req.user_prompt = std::move(user_prompt);
        req.thinking_level = config.thinking_level;
        req.thinking_budget = kDefaultThinkingBudgetTokens;
        req.max_output_tokens = kDefaultMaxOutputTokens;
        req.json_mode = true;
        return ensure_transport().generate(req);
    }

    fs::path default_checkpoint_path() const {
        if (!output_json_path.empty()) {
            return path_from_utf8(output_json_path);
        }
        return path_from_utf8(config.game_dir) / kScriptOutputDir /
               kDefaultCheckpointFilename;
    }

    Result<SpotCheckReport> run_scan_only() {
        const HotspotDetector detector(make_hotspot_config(config));
        SpotCheckReport rep;
        rep.total_entries = entries.size();

        if (!entries.empty()) {
            const OlsModel ols = detector.fit_length_regression(entries);
            rep.ols_slope = ols.beta;
            rep.ols_intercept = ols.alpha;
            rep.ols_sigma = ols.sigma;
            rep.ols_valid = ols.valid;
            rep.ranked_hotspots = detector.select_hotspots(entries, nullptr);
            rep.hotspots_detected = rep.ranked_hotspots.size();
        }

        if (!output_json_path.empty()) {
            const CheckpointState state =
                build_checkpoint_state(config, rep, {});
            const Status save_st = save_checkpoint_atomic(
                *file_store, path_from_utf8(output_json_path), state);
            if (!save_st.is_ok()) {
                return Result<SpotCheckReport>::failure(save_st);
            }
        }

        log_sink->info("Scan-only complete: " +
                       std::to_string(rep.total_entries) + " entries, " +
                       std::to_string(rep.hotspots_detected) +
                       " hotspots detected (0 API calls)");
        return Result<SpotCheckReport>::success(std::move(rep));
    }

    Result<SpotCheckReport> run() {
        if (config.scan_only) {
            return run_scan_only();
        }

        const fs::path ckpt_path = default_checkpoint_path();
        const fs::path game_root = path_from_utf8(config.game_dir);

        Result<std::optional<CheckpointState>> prev_res =
            load_checkpoint_if_exists(*file_store, ckpt_path, config.window_size);
        if (!prev_res.is_ok()) {
            return Result<SpotCheckReport>::failure(prev_res.status());
        }

        std::vector<ScriptEntry> working_entries = entries;
        SpotCheckReport rep;
        rep.total_entries = working_entries.size();

        std::map<std::size_t, Improvement> merged;
        std::set<std::size_t> repaired_indices;
        std::set<std::size_t> visited_offsets;
        IntervalTracker checked_tracker;
        IntervalTracker skip_tracker;
        BlockedSpanBookkeeper blocked_bookkeeper;
        std::vector<std::pair<std::size_t, std::size_t>> requeue_blocked_spans;
        std::vector<RadarPassageResult> radar_results;
        bool needs_artifact_sync = false;

        if (prev_res.value().has_value()) {
            const CheckpointState& prev = *prev_res.value();
            rep.h_windows_skipped = prev.h_windows_skipped;
            rep.radar_calls_made = prev.radar_calls_made;
            rep.radar_passages_triaged = prev.radar_passages_triaged;
            rep.adaptive_expansions = prev.adaptive_expansions;
            rep.windows_checked = prev.windows_checked;
            rep.lines_checked = prev.lines_checked;
            rep.editable_lines_unlocked = prev.editable_lines_unlocked;
            rep.candidates_rejected_by_validator =
                prev.candidates_rejected_by_validator;
            rep.script_entries_updated = prev.script_entries_updated;
            rep.cache_entries_updated = prev.cache_entries_updated;
            rep.token_usage = prev.token_usage;

            for (std::size_t off : prev.checked_offsets) {
                visited_offsets.insert(off);
            }
            for (const auto& [span_off, span_len] : prev.checked_spans) {
                checked_tracker.mark_visited(span_off, span_len);
                skip_tracker.mark_visited(span_off, span_len);
            }

            for (const auto& [b_off, b_len] : prev.blocked_spans) {
                if (b_len == 0) {
                    continue;
                }
                if (!working_entries.empty()) {
                    if (b_off >= working_entries.size()) {
                        continue;
                    }
                    const std::size_t clamped_len =
                        std::min(b_len, working_entries.size() - b_off);
                    blocked_bookkeeper.record_blocked_leaf(b_off, clamped_len);
                } else {
                    blocked_bookkeeper.record_blocked_leaf(b_off, b_len);
                }
            }
            for (const auto& [span_off, span_len] : prev.checked_spans) {
                blocked_bookkeeper.clear_span(span_off, span_len);
            }

            if (prev.model == config.model) {
                for (const auto& [b_off, b_len] : blocked_bookkeeper.spans()) {
                    skip_tracker.mark_visited(b_off, b_len);
                }
            } else {
                requeue_blocked_spans = blocked_bookkeeper.spans();
            }

            radar_results = prev.radar_results;

            for (const Improvement& imp : prev.improvements) {
                merged[imp.global_index] = imp;
                repaired_indices.insert(imp.global_index);
                if (imp.global_index < working_entries.size() &&
                    working_entries[imp.global_index].jp_text == imp.jp_text) {
                    working_entries[imp.global_index].current_en = imp.improved_en;
                    entries[imp.global_index].current_en = imp.improved_en;
                }
            }
            needs_artifact_sync = !merged.empty();
        }

        if (working_entries.empty()) {
            sync_report_collections(
                visited_offsets, checked_tracker, blocked_bookkeeper, merged, rep);
            const Status save_st = save_checkpoint_atomic(
                *file_store,
                ckpt_path,
                build_checkpoint_state(config, rep, radar_results));
            if (!save_st.is_ok()) {
                return Result<SpotCheckReport>::failure(save_st);
            }
            return Result<SpotCheckReport>::success(std::move(rep));
        }

        std::vector<bool> h_scene_mask;
        IntervalTracker h_scene_tracker;
        if (config.skip_h_scenes) {
            const auto h_spans = detect_h_scene_spans(working_entries);
            h_scene_mask.assign(working_entries.size(), false);
            for (const auto& [h_off, h_len] : h_spans) {
                h_scene_tracker.mark_visited(h_off, h_len);
                const std::size_t h_end = std::min(
                    working_entries.size(), saturated_add(h_off, h_len));
                for (std::size_t idx = h_off; idx < h_end; ++idx) {
                    h_scene_mask[idx] = true;
                }
            }
        }
        const std::vector<bool>* const h_mask_ptr =
            config.skip_h_scenes ? &h_scene_mask : nullptr;

        const HotspotDetector detector(make_hotspot_config(config));
        const OlsModel ols_model =
            detector.fit_length_regression(working_entries);
        rep.ols_slope = ols_model.beta;
        rep.ols_intercept = ols_model.alpha;
        rep.ols_sigma = ols_model.sigma;
        rep.ols_valid = ols_model.valid;

        std::vector<LineWeaknessScore> novel_scores;
        std::deque<WorkItem> work_queue;

        if (config.mode == SpotCheckMode::Hotspot) {
            rep.ranked_hotspots =
                detector.select_hotspots(working_entries, &novel_scores);
            rep.hotspots_detected = rep.ranked_hotspots.size();
            for (const HotspotWindow& hw : rep.ranked_hotspots) {
                const auto uncovered =
                    extract_uncovered_spans(skip_tracker, hw.offset, hw.length);
                for (const auto& [u_off, u_len] : uncovered) {
                    work_queue.push_back(WorkItem{u_off, u_len, false});
                }
            }
            log_sink->info(
                "Hotspot scan complete: " +
                std::to_string(rep.hotspots_detected) +
                " candidate windows (" +
                std::to_string(visited_offsets.size()) +
                " previously checked, " +
                std::to_string(repaired_indices.size()) +
                " lines already repaired)");
        } else {
            novel_scores = detector.score_entries(working_entries);
            const bool has_step_cap = (config.max_windows > 0);
            const std::size_t linear_step_budget =
                (config.max_windows > rep.windows_checked)
                    ? (config.max_windows - rep.windows_checked)
                    : 0U;
            std::size_t queued_linear_steps = 0;
            std::size_t off = config.offset;
            while (off < working_entries.size()) {
                if (has_step_cap && queued_linear_steps >= linear_step_budget) {
                    break;
                }
                const std::size_t remaining = working_entries.size() - off;
                const std::size_t len = std::min(config.window_size, remaining);
                const auto uncovered =
                    extract_uncovered_spans(skip_tracker, off, len);
                if (!uncovered.empty()) {
                    for (const auto& [u_off, u_len] : uncovered) {
                        work_queue.push_back(WorkItem{u_off, u_len, false});
                    }
                    ++queued_linear_steps;
                }
                if (remaining <= config.window_size) {
                    break;
                }
                off += config.window_size;
            }
        }

        std::unordered_set<std::size_t> radar_suspect_globals;
        std::set<std::size_t> triaged_offsets;
        std::vector<RadarPassageResult> flagged_passages;
        for (const RadarPassageResult& rpr : radar_results) {
            triaged_offsets.insert(rpr.offset);
            for (int sid : rpr.suspect_ids) {
                if (sid >= 1) {
                    const std::size_t g_idx =
                        rpr.offset + static_cast<std::size_t>(sid - 1);
                    if (g_idx < working_entries.size()) {
                        radar_suspect_globals.insert(g_idx);
                    }
                }
            }
            if ((rpr.mqm_score > 0.0 || !rpr.suspect_ids.empty()) &&
                !skip_tracker.is_fully_covered(rpr.offset, rpr.length)) {
                flagged_passages.push_back(rpr);
            }
        }

        ScriptRepository repo(*file_store, *crypt_sync);
        const auto flush_pending_sync_on_abort = [&]() {
            if (!config.auto_apply || merged.empty() || !needs_artifact_sync) {
                return;
            }
            sync_report_collections(
                visited_offsets, checked_tracker, blocked_bookkeeper, merged, rep);
            Result<SyncCounts> sync_res =
                repo.apply_improvements_to_artifacts(game_root, rep.improvements);
            if (!sync_res.is_ok()) {
                return;
            }
            rep.script_entries_updated +=
                sync_res.value().script_entries_updated;
            rep.cache_entries_updated +=
                sync_res.value().cache_entries_updated;
            needs_artifact_sync = false;
            (void)save_checkpoint_atomic(
                *file_store,
                ckpt_path,
                build_checkpoint_state(config, rep, radar_results));
        };
        const auto persist_checkpoint_only = [&]() -> Status {
            sync_report_collections(
                visited_offsets, checked_tracker, blocked_bookkeeper, merged, rep);
            return save_checkpoint_atomic(
                *file_store,
                ckpt_path,
                build_checkpoint_state(config, rep, radar_results));
        };
        const auto persist_window_progress = [&]() -> Status {
            Status save_st = persist_checkpoint_only();
            if (!save_st.is_ok()) {
                return save_st;
            }
            if (config.auto_apply && config.periodic_sync_interval > 0 &&
                needs_artifact_sync && rep.windows_checked > 0 &&
                (rep.windows_checked % config.periodic_sync_interval == 0)) {
                Result<SyncCounts> sync_res =
                    repo.apply_improvements_to_artifacts(game_root,
                                                         rep.improvements);
                if (!sync_res.is_ok()) {
                    return sync_res.status();
                }
                rep.script_entries_updated +=
                    sync_res.value().script_entries_updated;
                rep.cache_entries_updated +=
                    sync_res.value().cache_entries_updated;
                needs_artifact_sync = false;
                save_st = persist_checkpoint_only();
                if (!save_st.is_ok()) {
                    return save_st;
                }
            }
            return Status::ok();
        };

        std::size_t run_api_calls = 0;
        const bool window_budget_open =
            (config.max_windows == 0 || rep.windows_checked < config.max_windows);

        if (window_budget_open &&
            config.radar_calls > rep.radar_calls_made &&
            !work_queue.empty()) {
            std::vector<std::pair<std::size_t, std::size_t>> unflagged_passages;

            for (std::size_t r = rep.radar_calls_made;
                 r < config.radar_calls && !work_queue.empty();
                 ++r) {
                if (config.max_api_calls > 0 &&
                    run_api_calls >= config.max_api_calls) {
                    break;
                }

                std::vector<WindowSlice> raw_pack;
                std::vector<WindowSlice> prompt_pack;
                raw_pack.reserve(config.radar_pack_size);
                prompt_pack.reserve(config.radar_pack_size);

                while (raw_pack.size() < config.radar_pack_size &&
                       !work_queue.empty()) {
                    const WorkItem cand = work_queue.front();
                    work_queue.pop_front();

                    const auto uncovered = extract_uncovered_spans(
                        skip_tracker, cand.offset, cand.length);
                    if (uncovered.empty()) {
                        continue;
                    }
                    for (std::size_t u = uncovered.size(); u > 1U; --u) {
                        work_queue.push_front(WorkItem{
                            uncovered[u - 1U].first,
                            uncovered[u - 1U].second,
                            cand.force_no_context});
                    }
                    auto [off, len] = uncovered.front();
                    if (off >= working_entries.size()) {
                        continue;
                    }
                    len = std::min(len, working_entries.size() - off);
                    if (len == 0) {
                        continue;
                    }

                    if (config.skip_h_scenes) {
                        if (h_scene_tracker.is_fully_covered(off, len)) {
                            skip_tracker.mark_visited(off, len);
                            ++rep.h_windows_skipped;
                            continue;
                        }
                        const auto non_h =
                            extract_uncovered_spans(h_scene_tracker, off, len);
                        if (non_h.empty()) {
                            continue;
                        }
                        for (std::size_t u = non_h.size(); u > 1U; --u) {
                            work_queue.push_front(WorkItem{
                                non_h[u - 1U].first,
                                non_h[u - 1U].second,
                                cand.force_no_context});
                        }
                        off = non_h.front().first;
                        len = non_h.front().second;
                    }

                    if (triaged_offsets.count(off) != 0) {
                        bool is_flagged = false;
                        for (const RadarPassageResult& fp : flagged_passages) {
                            if (fp.offset == off) {
                                is_flagged = true;
                                break;
                            }
                        }
                        if (!is_flagged) {
                            unflagged_passages.emplace_back(off, len);
                        }
                        continue;
                    }

                    WindowSlice ws = build_window_slice(
                        working_entries, off, len, 0, 0, h_mask_ptr);
                    if (ws.target.empty()) {
                        continue;
                    }

                    WindowSlice p_ws = config.skip_h_scenes
                                           ? redact_h_lines_for_prompt(
                                                 ws, h_scene_mask)
                                           : ws;
                    raw_pack.push_back(std::move(ws));
                    prompt_pack.push_back(std::move(p_ws));
                }

                if (raw_pack.empty()) {
                    break;
                }

                log_sink->info("[Radar " + std::to_string(r + 1) + "/" +
                               std::to_string(config.radar_calls) +
                               "] Triaging " + std::to_string(raw_pack.size()) +
                               " candidate passages...");

                Result<LlmResponse> llm_res =
                    invoke_llm(build_radar_pack_prompt(prompt_pack, glossary));
                if (!llm_res.is_ok()) {
                    flush_pending_sync_on_abort();
                    return Result<SpotCheckReport>::failure(llm_res.status());
                }
                record_llm_usage(
                    llm_res.value(), true, run_api_calls, rep.token_usage);
                ++rep.radar_calls_made;
                rep.radar_passages_triaged += raw_pack.size();

                if (llm_res.value().content_blocked) {
                    const std::string_view reason =
                        llm_res.value().finish_reason.empty()
                            ? kDefaultBlockReason
                            : std::string_view(llm_res.value().finish_reason);
                    log_sink->warn(
                        "[Radar " + std::to_string(r + 1) + "/" +
                        std::to_string(config.radar_calls) +
                        "] Content blocked (" + std::string(reason) +
                        "); falling back to statistical hotspot order");
                    for (const WindowSlice& ws : raw_pack) {
                        triaged_offsets.insert(ws.target_start);
                        unflagged_passages.emplace_back(
                            ws.target_start, ws.target.size());
                    }
                    const Status blocked_save_st = persist_checkpoint_only();
                    if (!blocked_save_st.is_ok()) {
                        return Result<SpotCheckReport>::failure(blocked_save_st);
                    }
                    continue;
                }

                Result<std::vector<RadarPassageResult>> batch_res =
                    parse_radar_pack_response(llm_res.value().text, raw_pack);
                if (!batch_res.is_ok()) {
                    flush_pending_sync_on_abort();
                    return Result<SpotCheckReport>::failure(batch_res.status());
                }

                std::set<std::size_t> flagged_in_batch;
                for (const RadarPassageResult& rpr : batch_res.value()) {
                    radar_results.push_back(rpr);
                    triaged_offsets.insert(rpr.offset);

                    const WindowSlice& orig_ws =
                        raw_pack[static_cast<std::size_t>(rpr.passage_id - 1)];
                    for (int sid : rpr.suspect_ids) {
                        if (sid >= 1 &&
                            static_cast<std::size_t>(sid) <= orig_ws.target.size()) {
                            radar_suspect_globals.insert(
                                orig_ws.target[static_cast<std::size_t>(sid - 1)]
                                    .global_index);
                        }
                    }

                    if (rpr.mqm_score > 0.0 || !rpr.suspect_ids.empty()) {
                        flagged_in_batch.insert(rpr.offset);
                        flagged_passages.push_back(rpr);
                    }
                }

                for (const WindowSlice& ws : raw_pack) {
                    triaged_offsets.insert(ws.target_start);
                    if (flagged_in_batch.count(ws.target_start) == 0) {
                        unflagged_passages.emplace_back(
                            ws.target_start, ws.target.size());
                    }
                }

                const Status save_st = persist_checkpoint_only();
                if (!save_st.is_ok()) {
                    return Result<SpotCheckReport>::failure(save_st);
                }
            }

            for (auto it = unflagged_passages.rbegin();
                 it != unflagged_passages.rend();
                 ++it) {
                work_queue.push_front(WorkItem{it->first, it->second, false});
            }
        }

        if (!flagged_passages.empty()) {
            std::sort(flagged_passages.begin(),
                      flagged_passages.end(),
                      [](const RadarPassageResult& a,
                         const RadarPassageResult& b) {
                          if (std::abs(a.mqm_score - b.mqm_score) >
                              kMqmSortTieEpsilon) {
                              return a.mqm_score > b.mqm_score;
                          }
                          return a.offset < b.offset;
                      });
            for (auto it = flagged_passages.rbegin();
                 it != flagged_passages.rend();
                 ++it) {
                const auto uncovered = extract_uncovered_spans(
                    skip_tracker, it->offset, it->length);
                for (auto uit = uncovered.rbegin();
                     uit != uncovered.rend();
                     ++uit) {
                    work_queue.push_front(
                        WorkItem{uit->first, uit->second, false});
                }
            }
        }

        if (!requeue_blocked_spans.empty()) {
            for (auto it = requeue_blocked_spans.rbegin();
                 it != requeue_blocked_spans.rend();
                 ++it) {
                const auto uncovered = extract_uncovered_spans(
                    skip_tracker, it->first, it->second);
                for (auto uit = uncovered.rbegin();
                     uit != uncovered.rend();
                     ++uit) {
                    work_queue.push_front(
                        WorkItem{uit->first, uit->second, false});
                }
            }
        }

        const ValidatorOptions validator_opts{
            config.min_confidence, config.fix_punctuation};

        while (!work_queue.empty()) {
            if (config.max_windows > 0 &&
                rep.windows_checked >= config.max_windows) {
                break;
            }

            const WorkItem item = work_queue.front();
            work_queue.pop_front();

            const auto uncovered = extract_uncovered_spans(
                skip_tracker, item.offset, item.length);
            if (uncovered.empty()) {
                continue;
            }
            for (std::size_t u = uncovered.size(); u > 1U; --u) {
                work_queue.push_front(WorkItem{
                    uncovered[u - 1U].first,
                    uncovered[u - 1U].second,
                    item.force_no_context});
            }
            auto [cur_offset, cur_len] = uncovered.front();
            if (cur_offset >= working_entries.size()) {
                continue;
            }
            cur_len = std::min(cur_len, working_entries.size() - cur_offset);
            if (cur_len == 0) {
                continue;
            }

            if (config.skip_h_scenes) {
                if (h_scene_tracker.is_fully_covered(cur_offset, cur_len)) {
                    skip_tracker.mark_visited(cur_offset, cur_len);
                    ++rep.h_windows_skipped;
                    continue;
                }
                const auto non_h = extract_uncovered_spans(
                    h_scene_tracker, cur_offset, cur_len);
                if (non_h.empty()) {
                    continue;
                }
                for (std::size_t u = non_h.size(); u > 1U; --u) {
                    work_queue.push_front(WorkItem{
                        non_h[u - 1U].first,
                        non_h[u - 1U].second,
                        item.force_no_context});
                }
                cur_offset = non_h.front().first;
                cur_len = non_h.front().second;
            }

            const std::size_t req_before =
                item.force_no_context ? 0U : config.context_before;
            const std::size_t req_after =
                item.force_no_context ? 0U : config.context_after;

            WindowSlice slice = build_window_slice(working_entries,
                                                   cur_offset,
                                                   cur_len,
                                                   req_before,
                                                   req_after,
                                                   h_mask_ptr);
            if (slice.target.empty()) {
                continue;
            }

            slice.editable_ids = compute_editable_ids(slice,
                                                      novel_scores,
                                                      config.review_all_in_window,
                                                      config.skip_h_scenes,
                                                      repaired_indices,
                                                      h_mask_ptr);

            if (!config.review_all_in_window && !radar_suspect_globals.empty()) {
                const std::size_t target_count = slice.target.size();
                for (std::size_t i = 0; i < target_count; ++i) {
                    if (radar_suspect_globals.count(
                            slice.target[i].global_index) == 0) {
                        continue;
                    }
                    if (is_locked_for_radar_unlock(slice.target[i],
                                                   config.skip_h_scenes,
                                                   repaired_indices,
                                                   h_mask_ptr)) {
                        continue;
                    }
                    if (i > 0 &&
                        !is_locked_for_radar_unlock(slice.target[i - 1],
                                                    config.skip_h_scenes,
                                                    repaired_indices,
                                                    h_mask_ptr)) {
                        slice.editable_ids.insert(static_cast<int>(i));
                    }
                    slice.editable_ids.insert(static_cast<int>(i + 1));
                    if (i + 1 < target_count &&
                        !is_locked_for_radar_unlock(slice.target[i + 1],
                                                    config.skip_h_scenes,
                                                    repaired_indices,
                                                    h_mask_ptr)) {
                        slice.editable_ids.insert(static_cast<int>(i + 2));
                    }
                }
            }

            WindowSlice prompt_slice =
                config.skip_h_scenes
                    ? redact_h_lines_for_prompt(slice, h_scene_mask)
                    : slice;
            slice.editable_ids = prompt_slice.editable_ids;

            if (slice.editable_ids.empty()) {
                checked_tracker.mark_visited(cur_offset, slice.target.size());
                skip_tracker.mark_visited(cur_offset, slice.target.size());
                blocked_bookkeeper.clear_span(cur_offset, slice.target.size());
                visited_offsets.insert(cur_offset);
                continue;
            }

            if (config.max_api_calls > 0 &&
                run_api_calls >= config.max_api_calls) {
                break;
            }

            Result<LlmResponse> llm_res = invoke_llm(
                build_spot_check_user_prompt(prompt_slice, config.fix_punctuation));
            if (!llm_res.is_ok()) {
                flush_pending_sync_on_abort();
                return Result<SpotCheckReport>::failure(llm_res.status());
            }
            record_llm_usage(
                llm_res.value(), false, run_api_calls, rep.token_usage);

            if (llm_res.value().content_blocked &&
                (!prompt_slice.context_before.empty() ||
                 !prompt_slice.context_after.empty())) {
                const std::string_view ctx_reason =
                    llm_res.value().finish_reason.empty()
                        ? kDefaultBlockReason
                        : std::string_view(llm_res.value().finish_reason);
                if (config.max_api_calls > 0 &&
                    run_api_calls >= config.max_api_calls) {
                    log_sink->warn(
                        "[Window offset=" + std::to_string(cur_offset) +
                        ", len=" + std::to_string(slice.target.size()) +
                        "] Content blocked with context (" +
                        std::string(ctx_reason) +
                        "); max_api_calls reached before context-stripped retry");
                    break;
                }
                log_sink->warn(
                    "[Window offset=" + std::to_string(cur_offset) +
                    ", len=" + std::to_string(slice.target.size()) +
                    "] Content blocked with context (" +
                    std::string(ctx_reason) +
                    "); retrying target window without surrounding context");
                slice.context_before.clear();
                slice.context_after.clear();
                prompt_slice.context_before.clear();
                prompt_slice.context_after.clear();

                llm_res = invoke_llm(build_spot_check_user_prompt(
                    prompt_slice, config.fix_punctuation));
                if (!llm_res.is_ok()) {
                    flush_pending_sync_on_abort();
                    return Result<SpotCheckReport>::failure(llm_res.status());
                }
                record_llm_usage(
                    llm_res.value(), false, run_api_calls, rep.token_usage);
            }

            if (llm_res.value().content_blocked) {
                const std::string_view reason =
                    llm_res.value().finish_reason.empty()
                        ? kDefaultBlockReason
                        : std::string_view(llm_res.value().finish_reason);
                const std::size_t target_len = slice.target.size();
                if (target_len > kDefaultMinBlockedSplitLines) {
                    const std::size_t left_len =
                        target_len / kBisectHalfDivisor;
                    const std::size_t right_offset = cur_offset + left_len;
                    const std::size_t right_len = target_len - left_len;
                    log_sink->warn(
                        "[Window offset=" + std::to_string(cur_offset) +
                        ", len=" + std::to_string(target_len) +
                        "] Content blocked without context (" +
                        std::string(reason) + "); bisecting into [" +
                        std::to_string(cur_offset) + ", +" +
                        std::to_string(left_len) + ") and [" +
                        std::to_string(right_offset) + ", +" +
                        std::to_string(right_len) + ")");
                    work_queue.push_front(
                        WorkItem{right_offset, right_len, true});
                    work_queue.push_front(
                        WorkItem{cur_offset, left_len, true});
                    const Status split_save_st = persist_checkpoint_only();
                    if (!split_save_st.is_ok()) {
                        return Result<SpotCheckReport>::failure(split_save_st);
                    }
                    continue;
                }

                log_sink->warn(
                    "[Window offset=" + std::to_string(cur_offset) +
                    ", len=" + std::to_string(target_len) +
                    "] Leaf span persistently content-blocked (" +
                    std::string(reason) + "); recording in blocked_spans");
                blocked_bookkeeper.record_blocked_leaf(cur_offset, target_len);
                skip_tracker.mark_visited(cur_offset, target_len);
                const Status blocked_save_st = persist_checkpoint_only();
                if (!blocked_save_st.is_ok()) {
                    return Result<SpotCheckReport>::failure(blocked_save_st);
                }
                continue;
            }

            rep.editable_lines_unlocked += slice.editable_ids.size();

            Result<ParsedSpotCheckBatch> parsed_batch =
                parse_spot_check_response(
                    llm_res.value().text, slice, config.min_confidence);
            if (!parsed_batch.is_ok()) {
                flush_pending_sync_on_abort();
                return Result<SpotCheckReport>::failure(parsed_batch.status());
            }
            rep.candidates_rejected_by_validator +=
                parsed_batch.value().rejected_in_parse;

            std::vector<Improvement> accepted_new;
            for (Improvement& candidate : parsed_batch.value().candidates) {
                if (candidate.global_index >= working_entries.size() ||
                    repaired_indices.count(candidate.global_index) != 0) {
                    ++rep.candidates_rejected_by_validator;
                    continue;
                }

                const ScriptEntry& orig_entry =
                    working_entries[candidate.global_index];
                const ValidationResult val = validate_proposed_fix(
                    orig_entry, candidate, glossary, ols_model, validator_opts);
                if (!val.accepted) {
                    ++rep.candidates_rejected_by_validator;
                    continue;
                }

                working_entries[candidate.global_index].current_en =
                    candidate.improved_en;
                merged[candidate.global_index] = candidate;
                repaired_indices.insert(candidate.global_index);
                accepted_new.push_back(std::move(candidate));
                needs_artifact_sync = true;
            }

            checked_tracker.mark_visited(cur_offset, slice.target.size());
            skip_tracker.mark_visited(cur_offset, slice.target.size());
            blocked_bookkeeper.clear_span(cur_offset, slice.target.size());
            visited_offsets.insert(cur_offset);
            ++rep.windows_checked;
            rep.lines_checked += slice.target.size();

            if (config.mode == SpotCheckMode::Hotspot && !accepted_new.empty()) {
                const NeighborExpansion exp =
                    detector.evaluate_contagion(cur_offset,
                                                slice.target.size(),
                                                working_entries.size(),
                                                accepted_new);
                if (exp.expand_right) {
                    const auto [r_off, r_len] =
                        clip_right_expansion_at_h_barrier(
                            exp.right_offset, exp.right_length, h_mask_ptr);
                    const auto r_uncovered =
                        extract_uncovered_spans(skip_tracker, r_off, r_len);
                    if (!r_uncovered.empty()) {
                        for (auto uit = r_uncovered.rbegin();
                             uit != r_uncovered.rend();
                             ++uit) {
                            work_queue.push_front(
                                WorkItem{uit->first, uit->second, false});
                        }
                        ++rep.adaptive_expansions;
                    }
                }
                if (exp.expand_left) {
                    const auto [l_off, l_len] =
                        clip_left_expansion_at_h_barrier(
                            exp.left_offset, exp.left_length, h_mask_ptr);
                    const auto l_uncovered =
                        extract_uncovered_spans(skip_tracker, l_off, l_len);
                    if (!l_uncovered.empty()) {
                        for (auto uit = l_uncovered.rbegin();
                             uit != l_uncovered.rend();
                             ++uit) {
                            work_queue.push_front(
                                WorkItem{uit->first, uit->second, false});
                        }
                        ++rep.adaptive_expansions;
                    }
                }
            }

            const Status window_save_st = persist_window_progress();
            if (!window_save_st.is_ok()) {
                return Result<SpotCheckReport>::failure(window_save_st);
            }

            for (const Improvement& imp : accepted_new) {
                if (imp.global_index < entries.size()) {
                    entries[imp.global_index].current_en = imp.improved_en;
                }
            }

            char progress_buf[kProgressBufferBytes]{};
            if (config.max_windows > 0) {
                const double pct =
                    kPercentMultiplier *
                    static_cast<double>(rep.windows_checked) /
                    static_cast<double>(config.max_windows);
                std::snprintf(
                    progress_buf,
                    sizeof(progress_buf),
                    "[Window %zu/%zu (%.1f%%)] offset=%zu unlocked=%zu -> +%zu fixes (total=%zu)",
                    rep.windows_checked,
                    config.max_windows,
                    pct,
                    cur_offset,
                    slice.editable_ids.size(),
                    accepted_new.size(),
                    rep.improvements_found);
            } else {
                std::snprintf(
                    progress_buf,
                    sizeof(progress_buf),
                    "[Window %zu] offset=%zu unlocked=%zu -> +%zu fixes (total=%zu)",
                    rep.windows_checked,
                    cur_offset,
                    slice.editable_ids.size(),
                    accepted_new.size(),
                    rep.improvements_found);
            }
            log_sink->info(progress_buf);
        }

        sync_report_collections(
            visited_offsets, checked_tracker, blocked_bookkeeper, merged, rep);

        if (config.auto_apply && !merged.empty() && needs_artifact_sync) {
            Result<SyncCounts> sync_res =
                repo.apply_improvements_to_artifacts(game_root, rep.improvements);
            if (!sync_res.is_ok()) {
                return Result<SpotCheckReport>::failure(sync_res.status());
            }
            rep.script_entries_updated +=
                sync_res.value().script_entries_updated;
            rep.cache_entries_updated +=
                sync_res.value().cache_entries_updated;
            needs_artifact_sync = false;
        }

        const Status final_save_st = save_checkpoint_atomic(
            *file_store,
            ckpt_path,
            build_checkpoint_state(config, rep, radar_results));
        if (!final_save_st.is_ok()) {
            return Result<SpotCheckReport>::failure(final_save_st);
        }

        return Result<SpotCheckReport>::success(std::move(rep));
    }

    Result<ApplyReport> apply_improvements() {
        const fs::path ckpt_path = default_checkpoint_path();
        if (!file_store->exists(ckpt_path)) {
            return Result<ApplyReport>::failure(
                ErrorCode::IoError,
                "Checkpoint file does not exist: " + path_to_utf8(ckpt_path));
        }

        Result<std::optional<CheckpointState>> ckpt_res =
            load_checkpoint_if_exists(*file_store, ckpt_path, config.window_size);
        if (!ckpt_res.is_ok()) {
            return Result<ApplyReport>::failure(ckpt_res.status());
        }
        if (!ckpt_res.value().has_value()) {
            return Result<ApplyReport>::failure(
                ErrorCode::IoError,
                "Checkpoint file disappeared: " + path_to_utf8(ckpt_path));
        }

        CheckpointState ckpt = std::move(*ckpt_res.value());
        ApplyReport report;
        report.improvements_loaded = ckpt.improvements.size();
        if (ckpt.improvements.empty()) {
            return Result<ApplyReport>::success(report);
        }

        ScriptRepository repo(*file_store, *crypt_sync);
        const fs::path game_root = path_from_utf8(config.game_dir);
        Result<SyncCounts> sync_res =
            repo.apply_improvements_to_artifacts(game_root, ckpt.improvements);
        if (!sync_res.is_ok()) {
            return Result<ApplyReport>::failure(sync_res.status());
        }

        for (const Improvement& imp : ckpt.improvements) {
            if (imp.global_index < entries.size() &&
                entries[imp.global_index].jp_text == imp.jp_text) {
                entries[imp.global_index].current_en = imp.improved_en;
            }
        }

        report.script_entries_updated = sync_res.value().script_entries_updated;
        report.cache_entries_updated = sync_res.value().cache_entries_updated;

        if (report.script_entries_updated > 0 ||
            report.cache_entries_updated > 0) {
            ckpt.script_entries_updated += report.script_entries_updated;
            ckpt.cache_entries_updated += report.cache_entries_updated;
            const Status save_st =
                save_checkpoint_atomic(*file_store, ckpt_path, ckpt);
            if (!save_st.is_ok()) {
                return Result<ApplyReport>::failure(save_st);
            }
        }

        return Result<ApplyReport>::success(report);
    }
};

SpotChecker::SpotChecker(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

Result<SpotChecker> SpotChecker::create(SpotCheckConfig config,
                                        SpotCheckerDependencies deps) {
    if (trim_ascii_view(config.game_dir).empty()) {
        return Result<SpotChecker>::failure(
            ErrorCode::InvalidArgument, "game_dir must not be empty");
    }
    if (config.window_size == 0) {
        return Result<SpotChecker>::failure(
            ErrorCode::InvalidArgument, "window_size must be >= 1");
    }
    if (config.radar_calls > 0 && config.radar_pack_size == 0) {
        return Result<SpotChecker>::failure(
            ErrorCode::InvalidArgument, "radar_pack_size must be >= 1");
    }
    if (!std::isfinite(config.min_confidence) ||
        config.min_confidence <= kMinValidConfidenceExclusive ||
        config.min_confidence > kMaxValidConfidenceInclusive) {
        return Result<SpotChecker>::failure(
            ErrorCode::InvalidArgument,
            "min_confidence must be finite and in (0.0, 1.0]");
    }
    if (!std::isfinite(config.z_threshold)) {
        return Result<SpotChecker>::failure(
            ErrorCode::InvalidArgument, "z_threshold must be finite");
    }

    if (vn::llm::is_disallowed_model(config.model)) {
        try {
            vn::llm::ensure_supported_model(config.model);
        } catch (const vn::llm::LlmError& ex) {
            return Result<SpotChecker>::failure(
                ErrorCode::InvalidArgument, ex.what());
        }
        return Result<SpotChecker>::failure(
            ErrorCode::InvalidArgument,
            "disallowed LLM model '" + std::string(trim_ascii_view(config.model)) +
                "': argon models are disabled in this repository");
    }
    const std::string_view trimmed_model = trim_ascii_view(config.model);
    if (trimmed_model.empty()) {
        config.model = std::string(kDefaultModel);
    } else {
        config.model = std::string(trimmed_model);
    }
    if (!is_valid_thinking_level(config.thinking_level)) {
        return Result<SpotChecker>::failure(
            ErrorCode::InvalidArgument,
            "thinking_level must be MINIMAL, LOW, MEDIUM, HIGH, or empty");
    }
    config.thinking_level = normalize_thinking_level(config.thinking_level);

    auto impl = std::make_unique<Impl>();
    if (!config.scan_only && trim_ascii_view(config.out_path).empty()) {
        const fs::path default_out =
            path_from_utf8(config.game_dir) / kScriptOutputDir /
            kDefaultCheckpointFilename;
        config.out_path = path_to_utf8(default_out);
    }
    impl->output_json_path = config.out_path;
    impl->config = std::move(config);

    if (deps.clock != nullptr) {
        impl->clock = deps.clock;
    } else {
        impl->owned_clock = std::make_unique<SystemClock>();
        impl->clock = impl->owned_clock.get();
    }

    if (deps.log_sink != nullptr) {
        impl->log_sink = deps.log_sink;
    } else {
        impl->owned_log_sink = std::make_unique<ConsoleLogSink>();
        impl->log_sink = impl->owned_log_sink.get();
    }

    if (deps.file_store != nullptr) {
        impl->file_store = deps.file_store;
    } else {
        impl->owned_file_store = std::make_unique<DiskFileStore>();
        impl->file_store = impl->owned_file_store.get();
    }

    if (deps.crypt_sync != nullptr) {
        impl->crypt_sync = deps.crypt_sync;
    } else {
        impl->owned_crypt_sync = make_batch_crypt_sync({}, find_repo_root());
        impl->crypt_sync = impl->owned_crypt_sync.get();
    }

    impl->transport = deps.transport;

    const ScriptRepository repo(*impl->file_store, *impl->crypt_sync);
    Result<LoadedCorpus> loaded =
        repo.load_from_game_dir(path_from_utf8(impl->config.game_dir));
    if (!loaded.is_ok()) {
        return Result<SpotChecker>::failure(loaded.status());
    }

    impl->entries = std::move(loaded.value().entries);
    impl->glossary = std::move(loaded.value().glossary);
    impl->system_prompt =
        build_system_prompt(impl->glossary, impl->config.fix_punctuation);

    return Result<SpotChecker>::success(SpotChecker(std::move(impl)));
}

SpotChecker::~SpotChecker() = default;
SpotChecker::SpotChecker(SpotChecker&& other) noexcept = default;
SpotChecker& SpotChecker::operator=(SpotChecker&& other) noexcept = default;

Result<SpotCheckReport> SpotChecker::run() {
    return impl_->run();
}

Result<ApplyReport> SpotChecker::apply_improvements() {
    return impl_->apply_improvements();
}

const SpotCheckConfig& SpotChecker::config() const noexcept {
    return impl_->config;
}

const std::vector<ScriptEntry>& SpotChecker::entries() const noexcept {
    return impl_->entries;
}

const std::string& SpotChecker::system_prompt() const noexcept {
    return impl_->system_prompt;
}

const std::string& SpotChecker::output_json_path() const noexcept {
    return impl_->output_json_path;
}

}  // namespace spot_checker
