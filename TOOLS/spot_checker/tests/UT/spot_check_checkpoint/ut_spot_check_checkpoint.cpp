// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "checker/spot_check_checkpoint.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace spot_checker {
namespace {

constexpr double kFloatTol = 1e-9;

class FakeMemoryFileStore final : public IFileStore {
public:
    bool fail_reads = false;
    bool fail_writes = false;

    bool exists(const std::filesystem::path& path) const override {
        return files_.find(normalize(path)) != files_.end();
    }

    bool is_directory(const std::filesystem::path& path) const override {
        const std::string prefix = normalize(path) + "/";
        for (const auto& [key, unused_value] : files_) {
            (void)unused_value;
            if (key.rfind(prefix, 0U) == 0U) {
                return true;
            }
        }
        return false;
    }

    std::vector<std::filesystem::path> list_regular_files(
        const std::filesystem::path& dir) const override {
        const std::string prefix = normalize(dir) + "/";
        std::vector<std::filesystem::path> out;
        for (const auto& [key, unused_value] : files_) {
            (void)unused_value;
            if (key.rfind(prefix, 0U) == 0U &&
                key.find('/', prefix.size()) == std::string::npos) {
                out.emplace_back(key);
            }
        }
        return out;
    }

    Result<std::string> read_text(const std::filesystem::path& path) const override {
        if (fail_reads) {
            return Result<std::string>::failure(ErrorCode::IoError, "injected read fault");
        }
        const auto it = files_.find(normalize(path));
        if (it == files_.end()) {
            return Result<std::string>::failure(ErrorCode::IoError, "file not found");
        }
        return Result<std::string>::success(it->second);
    }

    Status write_text_atomic(const std::filesystem::path& path,
                             std::string_view content) override {
        if (fail_writes) {
            return Status::error(ErrorCode::IoError, "injected atomic write fault");
        }
        files_[normalize(path)] = std::string(content);
        return Status::ok();
    }

    void put_raw(const std::filesystem::path& path, std::string_view raw) {
        files_[normalize(path)] = std::string(raw);
    }

private:
    static std::string normalize(const std::filesystem::path& p) {
        return p.generic_string();
    }

    std::map<std::string, std::string> files_;
};

TEST(SpotCheckCheckpointTest, RoundTripsFullStateAndPinsShippedWireKeys) {
    FakeMemoryFileStore store;
    const std::filesystem::path path = "virtual_game/argon_spot_check.json";

    CheckpointState state;
    state.game_dir = "<game_dir>";
    state.model = "gemini-4-argon";
    state.mode = "hotspot";
    state.total_entries = 1200U;
    state.ols_slope = 1.95;
    state.ols_intercept = 4.5;
    state.ols_sigma = 11.25;
    state.ols_valid = true;
    state.hotspots_detected = 8U;
    state.h_windows_skipped = 2U;
    state.radar_calls_made = 3U;
    state.radar_passages_triaged = 18U;
    state.adaptive_expansions = 1U;
    state.windows_checked = 4U;
    state.lines_checked = 160U;
    state.windows_blocked = 2U;
    state.lines_blocked = 9U;
    state.editable_lines_unlocked = 52U;
    state.candidates_rejected_by_validator = 3U;
    state.script_entries_updated = 2U;
    state.cache_entries_updated = 1U;
    state.token_usage = TokenUsageSummary{7U, 3U, 4U, 14000, 1850, 920};
    state.checked_offsets = {0U, 120U};
    state.checked_spans = {{0U, 40U}, {120U, 40U}};
    state.blocked_offsets = {200U, 205U};
    state.blocked_spans = {{200U, 5U}, {205U, 4U}};

    RadarPassageResult radar;
    radar.passage_id = 1;
    radar.offset = 120U;
    radar.length = 40U;
    radar.critical_count = 1;
    radar.major_count = 2;
    radar.minor_count = 0;
    radar.mqm_score = 35.0;
    radar.suspect_ids = {4, 5};
    state.radar_results.push_back(radar);

    HotspotWindow win;
    win.offset = 120U;
    win.length = 40U;
    win.peak_index = 128U;
    win.energy = 19.5;
    win.cusum_peak = 12.0;
    win.z_score = 3.4;
    win.peak_line_score = 8.0;
    win.dominant_signal = "leaked_jp";
    state.ranked_hotspots.push_back(win);

    Improvement imp;
    imp.global_index = 124U;
    imp.script_name = "sc02";
    imp.entry_index = 14U;
    imp.speaker = "Kenji";
    imp.jp_text = "「あの時、助けてくれたよな。」";
    imp.old_en = "\"Back then, I helped you out.\"";
    imp.improved_en = "\"Back then, you helped me out.\"";
    imp.category = "mistranslation";
    imp.severity = "critical";
    imp.error_span = "I helped you";
    imp.fix_span = "you helped me";
    imp.confidence = 0.96;
    imp.reason = "Reversed benefactive direction.";
    state.improvements.push_back(imp);

    ASSERT_TRUE(save_checkpoint_atomic(store, path, state).is_ok());

    // Pin wire-level JSON keys required by downstream tools (e.g. showcase & --apply):
    const Result<std::string> raw_saved = store.read_text(path);
    ASSERT_TRUE(raw_saved.is_ok());
    EXPECT_NE(raw_saved.value().find("\"improved_en\""), std::string::npos);
    EXPECT_NE(raw_saved.value().find("\"new_en\""), std::string::npos);
    EXPECT_NE(raw_saved.value().find("\"improvements_count\""), std::string::npos);
    EXPECT_NE(raw_saved.value().find("\"checked_spans\""), std::string::npos);
    EXPECT_NE(raw_saved.value().find("\"windows_blocked\""), std::string::npos);
    EXPECT_NE(raw_saved.value().find("\"lines_blocked\""), std::string::npos);
    EXPECT_NE(raw_saved.value().find("\"blocked_offsets\""), std::string::npos);
    EXPECT_NE(raw_saved.value().find("\"blocked_spans\""), std::string::npos);
    EXPECT_NE(raw_saved.value().find("\"token_usage\""), std::string::npos);

    const Result<std::optional<CheckpointState>> loaded_opt =
        load_checkpoint_if_exists(store, path);
    ASSERT_TRUE(loaded_opt.is_ok());
    ASSERT_TRUE(loaded_opt.value().has_value());
    const CheckpointState& loaded = *loaded_opt.value();

    EXPECT_EQ(loaded.game_dir, "<game_dir>");
    EXPECT_EQ(loaded.model, "gemini-4-argon");
    EXPECT_EQ(loaded.mode, "hotspot");
    EXPECT_EQ(loaded.total_entries, 1200U);
    EXPECT_NEAR(loaded.ols_slope, 1.95, kFloatTol);
    EXPECT_NEAR(loaded.ols_intercept, 4.5, kFloatTol);
    EXPECT_NEAR(loaded.ols_sigma, 11.25, kFloatTol);
    EXPECT_TRUE(loaded.ols_valid);
    EXPECT_EQ(loaded.windows_checked, 4U);
    EXPECT_EQ(loaded.lines_checked, 160U);
    EXPECT_EQ(loaded.windows_blocked, 2U);
    EXPECT_EQ(loaded.lines_blocked, 9U);
    EXPECT_EQ(loaded.candidates_rejected_by_validator, 3U);
    EXPECT_EQ(loaded.token_usage.api_calls, 7U);
    EXPECT_EQ(loaded.token_usage.prompt_tokens, 14000);
    EXPECT_EQ(loaded.checked_offsets, (std::vector<std::size_t>{0U, 120U}));
    EXPECT_EQ(loaded.checked_spans,
              (std::vector<std::pair<std::size_t, std::size_t>>{{0U, 40U}, {120U, 40U}}));
    EXPECT_EQ(loaded.blocked_offsets, (std::vector<std::size_t>{200U, 205U}));
    EXPECT_EQ(loaded.blocked_spans,
              (std::vector<std::pair<std::size_t, std::size_t>>{{200U, 5U}, {205U, 4U}}));
    ASSERT_EQ(loaded.radar_results.size(), 1U);
    EXPECT_NEAR(loaded.radar_results[0].mqm_score, 35.0, kFloatTol);
    EXPECT_EQ(loaded.radar_results[0].suspect_ids, (std::set<int>{4, 5}));
    ASSERT_EQ(loaded.ranked_hotspots.size(), 1U);
    EXPECT_EQ(loaded.ranked_hotspots[0].offset, 120U);
    ASSERT_EQ(loaded.improvements.size(), 1U);
    EXPECT_EQ(loaded.improvements[0].global_index, 124U);
    EXPECT_EQ(loaded.improvements[0].improved_en, "\"Back then, you helped me out.\"");
    EXPECT_EQ(loaded.improvements[0].error_span, "I helped you");
    EXPECT_EQ(loaded.improvements[0].fix_span, "you helped me");
    EXPECT_NEAR(loaded.improvements[0].confidence, 0.96, kFloatTol);
}

TEST(SpotCheckCheckpointTest, SynthesizesCheckedAndBlockedSpansFromLegacyOffsets) {
    constexpr std::string_view kLegacyJson = R"({
        "game_dir": "<game_dir>",
        "mode": "linear",
        "total_entries": 500,
        "checked_offsets": [0, 80],
        "improvements": []
    })";

    const Result<CheckpointState> parsed = parse_checkpoint_json(kLegacyJson, 40U);
    ASSERT_TRUE(parsed.is_ok());
    EXPECT_EQ(parsed.value().mode, "linear");
    EXPECT_EQ(parsed.value().windows_blocked, 0U);
    EXPECT_EQ(parsed.value().lines_blocked, 0U);
    EXPECT_EQ(parsed.value().blocked_offsets.size(), 0U);
    EXPECT_EQ(parsed.value().blocked_spans.size(), 0U);
    ASSERT_EQ(parsed.value().checked_spans.size(), 2U);
    EXPECT_EQ(parsed.value().checked_spans[0], (std::pair<std::size_t, std::size_t>{0U, 40U}));
    EXPECT_EQ(parsed.value().checked_spans[1], (std::pair<std::size_t, std::size_t>{80U, 40U}));

    constexpr std::string_view kBlockedOffsetsOnlyJson = R"({
        "game_dir": "<game_dir>",
        "mode": "hotspot",
        "total_entries": 500,
        "windows_blocked": 2,
        "lines_blocked": 80,
        "blocked_offsets": [160, 240],
        "improvements": []
    })";
    const Result<CheckpointState> blocked_parsed =
        parse_checkpoint_json(kBlockedOffsetsOnlyJson, 40U);
    ASSERT_TRUE(blocked_parsed.is_ok());
    EXPECT_EQ(blocked_parsed.value().windows_blocked, 2U);
    EXPECT_EQ(blocked_parsed.value().lines_blocked, 80U);
    ASSERT_EQ(blocked_parsed.value().blocked_spans.size(), 2U);
    EXPECT_EQ(blocked_parsed.value().blocked_spans[0],
              (std::pair<std::size_t, std::size_t>{160U, 40U}));
    EXPECT_EQ(blocked_parsed.value().blocked_spans[1],
              (std::pair<std::size_t, std::size_t>{240U, 40U}));
}

TEST(SpotCheckCheckpointTest, HandlesMissingFileCorruptJsonAndIoFaultsCleanly) {
    FakeMemoryFileStore store;
    const std::filesystem::path missing_path = "virtual_game/missing.json";
    const Result<std::optional<CheckpointState>> missing =
        load_checkpoint_if_exists(store, missing_path);
    ASSERT_TRUE(missing.is_ok());
    EXPECT_FALSE(missing.value().has_value());

    constexpr std::string_view kCorruptCheckpoints[] = {
        "",
        "   \r\n ",
        "{broken_json",
        "[]",
        "123",
        "null",
        R"({"game_dir": "<game_dir>", "windows_blocked": "bad_type"})",
        R"({"game_dir": "<game_dir>", "blocked_offsets": "bad_type"})",
        R"({"game_dir": "<game_dir>", "blocked_spans": ["not_an_object"]})",
    };
    for (const std::string_view raw : kCorruptCheckpoints) {
        SCOPED_TRACE(raw);
        const Result<CheckpointState> err = parse_checkpoint_json(raw);
        EXPECT_FALSE(err.is_ok());
        EXPECT_EQ(err.error_code(), ErrorCode::CorruptCheckpoint);
    }

    const std::filesystem::path bad_path = "virtual_game/corrupt.json";
    store.put_raw(bad_path, "{malformed");
    const auto corrupt_load = load_checkpoint_if_exists(store, bad_path);
    EXPECT_FALSE(corrupt_load.is_ok());
    EXPECT_EQ(corrupt_load.error_code(), ErrorCode::CorruptCheckpoint);

    // Valid -> Failing write -> Valid read state preservation seam:
    const std::filesystem::path valid_path = "virtual_game/state.json";
    CheckpointState initial;
    initial.game_dir = "<game_dir>";
    initial.windows_checked = 5U;
    ASSERT_TRUE(save_checkpoint_atomic(store, valid_path, initial).is_ok());

    CheckpointState mutated = initial;
    mutated.windows_checked = 99U;
    store.fail_writes = true;
    const Status write_err = save_checkpoint_atomic(store, valid_path, mutated);
    EXPECT_FALSE(write_err.is_ok());
    EXPECT_EQ(write_err.code, ErrorCode::IoError);
    store.fail_writes = false;

    const auto preserved = load_checkpoint_if_exists(store, valid_path);
    ASSERT_TRUE(preserved.is_ok());
    ASSERT_TRUE(preserved.value().has_value());
    EXPECT_EQ(preserved.value()->windows_checked, 5U);

    store.fail_reads = true;
    const auto read_err = load_checkpoint_if_exists(store, valid_path);
    EXPECT_FALSE(read_err.is_ok());
    EXPECT_EQ(read_err.error_code(), ErrorCode::IoError);
}

}  // namespace
}  // namespace spot_checker
