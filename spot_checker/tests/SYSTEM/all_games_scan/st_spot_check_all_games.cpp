// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "common/utf8_text.h"
#include "common/win32_cli_env.h"
#include "st_process_harness.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace spot_checker {
namespace {

using test_support::TempSandbox;
using test_support::locate_installed_spot_check_exe;
using test_support::run_process_utf8;

constexpr std::size_t kMinExpectedTranslatedGames = 15;
constexpr std::size_t kMinEntriesForGuaranteedOls = 50;
constexpr DWORD kPerGameScanTimeoutMs = 120000;
constexpr double kOlsSlopeMin = 0.6;
constexpr double kOlsSlopeMax = 3.0;
constexpr double kOlsSigmaMin = 8.0;
constexpr double kOlsSigmaMax = 40.0;
constexpr double kFloatSortEpsilon = 1e-9;

bool has_non_empty_env_var(const wchar_t* name) noexcept {
    // `GetEnvironmentVariableW(..., nullptr, 0)` returns required wide chars
    // including the trailing NUL when present (`0` when absent, `1` when empty),
    // probing existence without reading any secret value into memory.
    return GetEnvironmentVariableW(name, nullptr, 0) > 1U;
}

bool any_game_has_plain_translated_json(
    const std::vector<std::filesystem::path>& games) {
    return std::any_of(
        games.begin(),
        games.end(),
        [](const std::filesystem::path& game_dir) {
            std::error_code ec;
            return std::filesystem::is_regular_file(
                game_dir / "script_output" / "translated_text.json", ec);
        });
}

bool has_decryption_key_available(
    const std::filesystem::path& repo_root) noexcept {
    std::error_code ec;
    return has_non_empty_env_var(L"TRANSLATIONS_KEY") ||
           std::filesystem::is_regular_file(repo_root / "translations_key.txt",
                                            ec);
}

using FileMetaMap =
    std::map<std::string,
             std::pair<std::uintmax_t, std::filesystem::file_time_type>>;

FileMetaMap snapshot_game_dir_surface(const std::filesystem::path& game_dir) {
    FileMetaMap meta;
    std::error_code ec;

    for (const auto& entry : std::filesystem::directory_iterator(game_dir, ec)) {
        if (entry.is_regular_file(ec)) {
            const std::string key =
                wide_to_utf8(entry.path().filename().wstring());
            meta.emplace(key,
                         std::make_pair(entry.file_size(ec),
                                        entry.last_write_time(ec)));
        }
    }

    const auto script_out = game_dir / "script_output";
    if (std::filesystem::is_directory(script_out, ec)) {
        for (const auto& entry :
             std::filesystem::directory_iterator(script_out, ec)) {
            if (entry.is_regular_file(ec)) {
                const std::string key =
                    "script_output/" +
                    wide_to_utf8(entry.path().filename().wstring());
                meta.emplace(key,
                             std::make_pair(entry.file_size(ec),
                                            entry.last_write_time(ec)));
            }
        }
    }
    return meta;
}

double json_number_as_double(const boost::json::value& val) {
    if (val.is_double()) {
        return val.as_double();
    }
    if (val.is_int64()) {
        return static_cast<double>(val.as_int64());
    }
    if (val.is_uint64()) {
        return static_cast<double>(val.as_uint64());
    }
    return 0.0;
}

std::vector<std::filesystem::path> discover_translated_game_dirs(
    const std::filesystem::path& repo_root) {
    std::vector<std::filesystem::path> games;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(repo_root, ec)) {
        if (!entry.is_directory(ec)) {
            continue;
        }
        const std::string name =
            wide_to_utf8(entry.path().filename().wstring());
        if (name.empty() || name[0] == '.' || name == "spot_checker" ||
            name == "build" || name == "install") {
            continue;
        }
        const auto script_out = entry.path() / "script_output";
        const bool has_plain = std::filesystem::is_regular_file(
            script_out / "translated_text.json", ec);
        const bool has_enc = std::filesystem::is_regular_file(
            script_out / "translated_text.json.enc", ec);
        if (has_plain || has_enc) {
            games.push_back(entry.path());
        }
    }

    std::sort(games.begin(), games.end(),
              [](const std::filesystem::path& a, const std::filesystem::path& b) {
                  return wide_to_utf8(a.filename().wstring()) <
                         wide_to_utf8(b.filename().wstring());
              });
    return games;
}

std::filesystem::path stage_encrypted_only_game(
    const TempSandbox& sandbox,
    const std::filesystem::path& real_game_dir,
    const std::string& game_name) {
    const auto staged_dir = sandbox.path() / "stage" / game_name;
    std::filesystem::create_directories(staged_dir / "script_output");

    std::error_code ec;
    std::filesystem::copy_file(
        real_game_dir / "script_output" / "translated_text.json.enc",
        staged_dir / "script_output" / "translated_text.json.enc",
        std::filesystem::copy_options::overwrite_existing,
        ec);

    for (const char* tone_file : {"TONE_OF_NOVEL.md", "TONE.md"}) {
        const auto src_tone = real_game_dir / tone_file;
        if (std::filesystem::is_regular_file(src_tone, ec)) {
            std::filesystem::copy_file(
                src_tone,
                staged_dir / tone_file,
                std::filesystem::copy_options::overwrite_existing,
                ec);
        }
    }
    return staged_dir;
}

TEST(SpotCheckAllGamesSystemTest,
     ScansEveryShippedGameCorpusReadOnlyWithValidOlsAndZeroApiCalls) {
    const auto exe = locate_installed_spot_check_exe();
    ASSERT_TRUE(std::filesystem::is_regular_file(exe))
        << "Missing staged packaged binary at: " << wide_to_utf8(exe.wstring());

    const auto repo_root = find_repo_root_from(executable_path());
    ASSERT_FALSE(repo_root.empty());
    ASSERT_TRUE(is_repo_root_dir(repo_root))
        << "Resolved directory is not repository root: "
        << wide_to_utf8(repo_root.wstring());

    const auto games = discover_translated_game_dirs(repo_root);
    ASSERT_GE(games.size(), kMinExpectedTranslatedGames)
        << "Expected at least " << kMinExpectedTranslatedGames
        << " translated games under repository root, found " << games.size();

    if (!any_game_has_plain_translated_json(games) &&
        !has_decryption_key_available(repo_root)) {
        GTEST_SKIP()
            << "Skipping all-games scan on unkeyed external checkout: 0 plain "
               "script_output/translated_text.json files exist and neither "
               "TRANSLATIONS_KEY environment variable nor "
               "<repo_root>/translations_key.txt is present.";
    }

    TempSandbox sandbox("st_all_games_scan");
    bool exercised_game_name_flag = false;

    for (const auto& game_dir : games) {
        const std::string game_name =
            wide_to_utf8(game_dir.filename().wstring());
        SCOPED_TRACE(game_name);

        const FileMetaMap before_meta = snapshot_game_dir_surface(game_dir);

        std::error_code ec;
        const bool has_plain_json = std::filesystem::is_regular_file(
            game_dir / "script_output" / "translated_text.json", ec);

        const auto report_path =
            sandbox.path() / "reports" / (game_name + "_scan.json");

        std::vector<std::string> cli_args;
        if (has_plain_json && !exercised_game_name_flag) {
            cli_args = {"--game", game_name, "--scan-only", "--out",
                        wide_to_utf8(report_path.wstring())};
            exercised_game_name_flag = true;
        } else if (has_plain_json) {
            cli_args = {"--game-dir", wide_to_utf8(game_dir.wstring()),
                        "--scan-only", "--out",
                        wide_to_utf8(report_path.wstring())};
        } else {
            const auto staged_dir =
                stage_encrypted_only_game(sandbox, game_dir, game_name);
            cli_args = {"--game-dir", wide_to_utf8(staged_dir.wstring()),
                        "--scan-only", "--out",
                        wide_to_utf8(report_path.wstring())};
        }

        const auto run = run_process_utf8(
            exe, cli_args, sandbox.path(), kPerGameScanTimeoutMs);

        ASSERT_FALSE(run.launch_failed) << run.launch_error;
        ASSERT_FALSE(run.timed_out) << "Timed out scanning " << game_name;
        ASSERT_EQ(run.exit_code, 0)
            << "Failed scanning " << game_name << "\nstderr:\n"
            << run.stderr_text << "\nstdout:\n"
            << run.stdout_text;
        EXPECT_TRUE(run.stderr_text.empty()) << run.stderr_text;

        ASSERT_TRUE(std::filesystem::is_regular_file(report_path));
        const std::string report_raw = TempSandbox::read_bytes(report_path);
        ASSERT_FALSE(report_raw.empty());

        const auto obj = boost::json::parse(report_raw).as_object();
        const auto total_entries =
            obj.at("total_entries").to_number<std::size_t>();
        EXPECT_GT(total_entries, 0U);

        const bool ols_valid = obj.at("ols_valid").as_bool();
        if (total_entries >= kMinEntriesForGuaranteedOls) {
            EXPECT_TRUE(ols_valid);
        }
        if (ols_valid) {
            const double slope = json_number_as_double(obj.at("ols_slope"));
            const double sigma = json_number_as_double(obj.at("ols_sigma"));
            EXPECT_TRUE(std::isfinite(slope));
            EXPECT_GE(slope, kOlsSlopeMin);
            EXPECT_LE(slope, kOlsSlopeMax);
            EXPECT_TRUE(std::isfinite(sigma));
            EXPECT_GE(sigma, kOlsSigmaMin);
            EXPECT_LE(sigma, kOlsSigmaMax);
        }

        EXPECT_EQ(obj.at("radar_calls_made").to_number<std::size_t>(), 0U);
        EXPECT_EQ(obj.at("windows_checked").to_number<std::size_t>(), 0U);
        EXPECT_EQ(obj.at("token_usage")
                      .as_object()
                      .at("api_calls")
                      .to_number<std::size_t>(),
                  0U);

        const auto& hotspots = obj.at("ranked_hotspots").as_array();
        double prev_z = 0.0;
        for (std::size_t idx = 0; idx < hotspots.size(); ++idx) {
            const auto& hs = hotspots[idx].as_object();
            const auto offset = hs.at("offset").to_number<std::size_t>();
            const auto length = hs.at("length").to_number<std::size_t>();
            const double energy = json_number_as_double(hs.at("energy"));
            const double z_score = json_number_as_double(hs.at("z_score"));

            EXPECT_LT(offset, total_entries);
            EXPECT_GT(length, 0U);
            EXPECT_LE(offset + length, total_entries);
            EXPECT_GT(energy, 0.0);
            EXPECT_TRUE(std::isfinite(z_score));
            EXPECT_GE(z_score, 0.0);
            if (idx > 0U) {
                EXPECT_LE(z_score, prev_z + kFloatSortEpsilon);
            }
            prev_z = z_score;
        }

        const FileMetaMap after_meta = snapshot_game_dir_surface(game_dir);
        EXPECT_EQ(before_meta, after_meta)
            << "Real game directory mutated during --scan-only: " << game_name;
    }
}

}  // namespace
}  // namespace spot_checker
