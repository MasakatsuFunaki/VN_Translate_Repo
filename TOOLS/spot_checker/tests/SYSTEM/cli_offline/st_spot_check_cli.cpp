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
#include <array>
#include <cstddef>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace spot_checker {
namespace {

using test_support::TempSandbox;
using test_support::locate_installed_spot_check_exe;
using test_support::run_process_utf8;

constexpr DWORD kCliTimeoutMs = 30000;
constexpr std::size_t kScene01RowCount = 16;
constexpr std::size_t kScene02RowCount = 8;
constexpr std::size_t kSyntheticEntryCount = kScene01RowCount + kScene02RowCount;
constexpr std::size_t kPastEndLinearOffset = 100;

struct CleanRow {
    std::string_view speaker;
    std::string_view jp;
    std::string_view en;
};

constexpr std::array<CleanRow, 12> kCleanRows{{
    {"蒼井 遥",
     "「おはよう、今日もいい天気だね」",
     "\"Good morning, the weather looks clear again today.\""},
    {"",
     "放課後の教室には夕陽が差し込み、静かな風が白いカーテンを揺らしていた。",
     "Evening sunlight slanted across the empty classroom after school, and a quiet breeze stirred the white curtains."},
    {"高瀬 蓮",
     "「次の電車が来るまでベンチで少し待とうか」",
     "\"Shall we wait on the bench for a bit until the next train arrives?\""},
    {"",
     "古い石段を登りきった先には小さな展望台があり、夕暮れ時の港町と遠くの灯台が一望できた。",
     "At the top of the worn stone steps stood a small observation deck overlooking the harbor town at dusk and the distant lighthouse."},
    {"蒼井 遥",
     "「熱い紅茶を淹れたから、冷めないうちに飲んでね」",
     "\"I brewed a pot of hot black tea, so drink up before it cools down.\""},
    {"",
     "廊下の突き当たりにある音楽室から、誰かが練習しているピアノの旋律がかすかに聞こえてくる。",
     "From the music room at the far end of the hallway drifted the faint melody of someone practicing the piano."},
    {"高瀬 蓮",
     "「明日の朝は七時に駅前へ集合すれば間に合うはずだ」",
     "\"Meeting in front of the station at seven tomorrow morning should get us there on time.\""},
    {"",
     "雨上がりの舗道には街灯の光が幾重にも反射し、通り過ぎる自転車のタイヤが水たまりを弾いていた。",
     "Streetlight reflections shimmered across the wet pavement after the rain while passing bicycle tires splashed through shallow puddles."},
    {"蒼井 遥",
     "「このノート、先週の授業でまとめた部分だけ貸してあげる」",
     "\"Here, you can borrow the section of this notebook from last week's lecture.\""},
    {"",
     "商店街の時計屋が八時の鐘を鳴らす頃には、買い物客の足音もすっかりまばらになっていた。",
     "By the time the clockmaker's shop on the arcade chimed eight, the footsteps of evening shoppers had grown sparse."},
    {"高瀬 蓮",
     "「傘を持ってきて正解だったな、急に雲行きが怪しくなってきた」",
     "\"Bringing an umbrella turned out to be the right call; those clouds darkened fast.\""},
    {"",
     "木製の本棚には背表紙の色褪せた辞書や古い天体図鑑が隙間なく並び、乾いた紙の匂いが漂っていた。",
     "Faded dictionaries and old star atlases lined every shelf of the wooden bookcase, filling the quiet room with the scent of dry paper."},
}};

std::string json_escape(std::string_view text) {
    return boost::json::serialize(boost::json::value(text));
}

std::string load_colocated_tone_markdown() {
    const auto path = executable_path().parent_path() / "tone_of_novel.md";
    const std::string bytes = TempSandbox::read_bytes(path);
    if (!bytes.empty()) {
        return bytes;
    }
    return "# Tone Guide\n<name_reference>\n| 蒼井 遥 | Haruka Aoi |\n"
           "| 高瀬 蓮 | Ren Takase |\n</name_reference>\n";
}

std::string build_multi_schema_corpus_crlf(bool inject_defects) {
    std::ostringstream oss;
    oss << "{\r\n"
        << " \"scene_01\": {\r\n"
        << "  \"strings\": [\r\n";
    for (std::size_t i = 0; i < 16U; ++i) {
        const auto& r = kCleanRows[i % kCleanRows.size()];
        std::string jp(r.jp);
        std::string en(r.en);
        if (inject_defects && i == 6U) {
            jp = "「星見学園の図書室にある鍵は机の上にありますよ」";
            en = "\"The Hoshimi Academy library 鍵 is under the chair.\"";
        }
        oss << "   {\r\n";
        if (r.speaker.empty()) {
            oss << "    \"speaker\": null,\r\n";
        } else {
            oss << "    \"speaker\": " << json_escape(r.speaker) << ",\r\n";
        }
        oss << "    \"text\": " << json_escape(jp) << ",\r\n"
            << "    \"translated\": " << json_escape(en) << ",\r\n"
            << "    \"translation\": " << json_escape(en) << "\r\n"
            << "   }" << (i + 1U < 16U ? "," : "") << "\r\n";
    }
    oss << "  ]\r\n"
        << " },\r\n"
        << " \"scene_02\": {\r\n"
        << "  \"lines\": [\r\n";
    for (std::size_t i = 0; i < 8U; ++i) {
        const auto& r = kCleanRows[(i + 3U) % kCleanRows.size()];
        std::string jp(r.jp);
        std::string en(r.en);
        if (inject_defects && i == 3U) {
            // Cross-script duplicate JP matching `scene_01[6]`.
            jp = "「星見学園の図書室にある鍵は机の上にありますよ」";
            en = "\"The Hoshimi Academy library 鍵 is under the chair.\"";
        }
        oss << "   {\r\n"
            << "    \"content\": " << json_escape(jp) << ",\r\n"
            << "    \"translated\": " << json_escape(en) << "\r\n"
            << "   }" << (i + 1U < 8U ? "," : "") << "\r\n";
    }
    oss << "  ]\r\n"
        << " }\r\n"
        << "}\r\n";
    return oss.str();
}

std::vector<std::string> snapshot_relative_files(
    const std::filesystem::path& root) {
    std::vector<std::string> result;
    std::error_code ec;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(root, ec)) {
        if (entry.is_regular_file(ec)) {
            result.push_back(
                wide_to_utf8(std::filesystem::relative(entry.path(), root, ec)
                                 .generic_wstring()));
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

TEST(SpotCheckCliSystemTest,
     HelpFlagAndTableDrivenInvalidCliArgumentsAreRejectedCleanly) {
    const auto exe = locate_installed_spot_check_exe();
    ASSERT_TRUE(std::filesystem::is_regular_file(exe))
        << "Missing staged packaged binary at: " << wide_to_utf8(exe.wstring());

    TempSandbox sandbox("st_cli_help_invalid");
    const auto valid_game_dir = sandbox.path() / "ValidGame";
    sandbox.write_bytes("ValidGame/script_output/translated_text.json",
                        build_multi_schema_corpus_crlf(false));
    const std::string game_dir_u8 = wide_to_utf8(valid_game_dir.wstring());

    // `--help` and `-h` must exit 0 with empty stderr and list the canonical 16 options.
    for (const char* flag : {"--help", "-h"}) {
        SCOPED_TRACE(flag);
        const auto help_run = run_process_utf8(exe, {flag}, sandbox.path(), kCliTimeoutMs);
        ASSERT_FALSE(help_run.launch_failed) << help_run.launch_error;
        ASSERT_FALSE(help_run.timed_out);
        EXPECT_EQ(help_run.exit_code, 0);
        EXPECT_TRUE(help_run.stderr_text.empty()) << help_run.stderr_text;

        for (std::string_view expected_opt : {
                 "--game",
                 "--game-dir",
                 "--mode",
                 "--offset",
                 "--scan-only",
                 "--dry-run",
                 "--apply",
                 "--out",
                 "--model",
                 "--thinking-level",
                 "--radar-calls",
                 "--window",
                 "--max-windows",
                 "--z-threshold",
                 "--min-confidence",
                 "--lock-non-suspects",
                 "--include-h-scenes",
                 "--fix-punctuation",
             }) {
            EXPECT_NE(help_run.stdout_text.find(expected_opt), std::string::npos)
                << "Missing expected option in --help output: " << expected_opt;
        }
        // Dropped flags must NOT appear in CLI help.
        EXPECT_EQ(help_run.stdout_text.find("--report-out"), std::string::npos);
        EXPECT_EQ(help_run.stdout_text.find("--max-api-calls"), std::string::npos);
        EXPECT_EQ(help_run.stdout_text.find("--endpoint-url"), std::string::npos);
    }

    struct InvalidCliCase {
        const char* name;
        std::vector<std::string> args;
    };

    const std::vector<InvalidCliCase> invalid_cases{
        {"missing_target_game", {"--scan-only"}},
        {"mutually_exclusive_game_and_game_dir",
         {"--game", "ValidGame", "--game-dir", game_dir_u8, "--scan-only"}},
        {"mutually_exclusive_scan_only_and_apply",
         {"--game-dir", game_dir_u8, "--scan-only", "--apply"}},
        {"mutually_exclusive_dry_run_and_apply",
         {"--game-dir", game_dir_u8, "--dry-run", "--apply"}},
        {"mutually_exclusive_scan_only_and_dry_run",
         {"--game-dir", game_dir_u8, "--scan-only", "--dry-run"}},
        {"invalid_mode_value",
         {"--game-dir", game_dir_u8, "--mode", "diagonal", "--scan-only"}},
        {"zero_window_size",
         {"--game-dir", game_dir_u8, "--window", "0", "--scan-only"}},
        {"negative_offset",
         {"--game-dir", game_dir_u8, "--offset", "-1", "--scan-only"}},
        {"negative_radar_calls",
         {"--game-dir", game_dir_u8, "--radar-calls", "-2", "--scan-only"}},
        {"negative_max_windows",
         {"--game-dir", game_dir_u8, "--max-windows", "-1", "--scan-only"}},
        {"empty_model_id",
         {"--game-dir", game_dir_u8, "--model", "", "--scan-only"}},
        {"invalid_thinking_level",
         {"--game-dir", game_dir_u8, "--thinking-level", "invalid_level",
          "--scan-only"}},
        {"zero_min_confidence",
         {"--game-dir", game_dir_u8, "--min-confidence", "0.0", "--scan-only"}},
        {"above_one_min_confidence",
         {"--game-dir", game_dir_u8, "--min-confidence", "1.25", "--scan-only"}},
        {"unknown_cli_option",
         {"--game-dir", game_dir_u8, "--nonexistent-option"}},
        {"nonexistent_game_dir",
         {"--game-dir", wide_to_utf8((sandbox.path() / "MissingDir").wstring()),
          "--scan-only"}},
    };

    for (const auto& tc : invalid_cases) {
        SCOPED_TRACE(tc.name);
        const auto res = run_process_utf8(exe, tc.args, sandbox.path(), kCliTimeoutMs);
        ASSERT_FALSE(res.launch_failed) << res.launch_error;
        ASSERT_FALSE(res.timed_out);
        EXPECT_NE(res.exit_code, 0);
        EXPECT_FALSE(res.stderr_text.empty());
    }
}

TEST(SpotCheckCliSystemTest,
     ScanOnlyAndGameNameResolutionProduceExactHotspotReportWithoutGameDirMutation) {
    const auto exe = locate_installed_spot_check_exe();
    ASSERT_TRUE(std::filesystem::is_regular_file(exe));

    TempSandbox sandbox("st_cli_scan_only");
    const auto game_dir = sandbox.path() / "SyntheticCliNovel";
    sandbox.write_bytes("SyntheticCliNovel/TONE_OF_NOVEL.md",
                        load_colocated_tone_markdown());
    const std::string initial_tt = build_multi_schema_corpus_crlf(true);
    const auto tt_path = sandbox.write_bytes(
        "SyntheticCliNovel/script_output/translated_text.json", initial_tt);

    const auto pre_files = snapshot_relative_files(game_dir);
    const auto report_path = sandbox.path() / "reports" / "cli_scan_out.json";

    // Run with `--game SyntheticCliNovel` from `sandbox.path()` CWD + `--scan-only` + `--out`.
    const auto res = run_process_utf8(
        exe,
        {"--game", "SyntheticCliNovel", "--scan-only", "--window", "8",
         "--z-threshold", "0.0", "--out", wide_to_utf8(report_path.wstring())},
        sandbox.path(),
        kCliTimeoutMs);

    ASSERT_FALSE(res.launch_failed) << res.launch_error;
    ASSERT_FALSE(res.timed_out);
    EXPECT_EQ(res.exit_code, 0) << "stderr: " << res.stderr_text;
    EXPECT_TRUE(res.stderr_text.empty()) << res.stderr_text;
    EXPECT_FALSE(res.stdout_text.empty());

    // Zero writes or temp files inside `SyntheticCliNovel`.
    EXPECT_EQ(snapshot_relative_files(game_dir), pre_files);
    EXPECT_EQ(TempSandbox::read_bytes(tt_path), initial_tt);

    ASSERT_TRUE(std::filesystem::is_regular_file(report_path));
    const auto obj =
        boost::json::parse(TempSandbox::read_bytes(report_path)).as_object();
    EXPECT_EQ(obj.at("total_entries").to_number<std::size_t>(),
              kSyntheticEntryCount);
    EXPECT_TRUE(obj.at("ols_valid").as_bool());
    EXPECT_GE(obj.at("hotspots_detected").to_number<std::size_t>(), 1U);
    EXPECT_EQ(obj.at("radar_calls_made").to_number<std::size_t>(), 0U);
    EXPECT_EQ(obj.at("windows_checked").to_number<std::size_t>(), 0U);
    EXPECT_EQ(obj.at("token_usage")
                  .as_object()
                  .at("api_calls")
                  .to_number<std::size_t>(),
              0U);
}

TEST(SpotCheckCliSystemTest,
     OfflineApplyDecryptsPropagatesFixesUpdatesCachesReEncryptsAndIsIdempotent) {
    const auto exe = locate_installed_spot_check_exe();
    ASSERT_TRUE(std::filesystem::is_regular_file(exe));

    TempSandbox sandbox("st_cli_apply_crypt");
    const auto game_dir = sandbox.path() / "EncryptedNovel";

    constexpr std::string_view kDefectJp =
        "「星見学園の図書室にある鍵は机の上にありますよ」";
    constexpr std::string_view kOldEn =
        "\"The Hoshimi Academy library 鍵 is under the chair.\"";
    constexpr std::string_view kNewEn =
        "\"The key to the Hoshimi Academy library is on top of the desk.\"";

    // Write ONLY `.json.enc` (no plain `translated_text.json`), forcing real
    // `BatchProcessCryptSync::decrypt_if_needed` subprocess execution!
    const std::string initial_tt = build_multi_schema_corpus_crlf(true);
    const auto enc_path = sandbox.write_bytes(
        "EncryptedNovel/script_output/translated_text.json.enc", initial_tt);

    const std::string initial_cache =
        "{\n  \"zz_first\": \"Keep first\",\n  " + json_escape(kDefectJp) + ": " +
        json_escape(kOldEn) + ",\n  \"aa_last\": \"Keep last\"\n}\n";
    const auto cache_path = sandbox.write_bytes(
        "EncryptedNovel/script_output/translation_cache_anthropic.json",
        initial_cache);

    // Pre-populated `argon_spot_check.json` checkpoint.
    boost::json::object imp;
    imp["global_index"] = 6;
    imp["script"] = "scene_01";
    imp["script_name"] = "scene_01";
    imp["entry_index"] = 6;
    imp["speaker"] = "Haruka Aoi";
    imp["jp_text"] = kDefectJp;
    imp["old_en"] = kOldEn;
    imp["improved_en"] = kNewEn;
    imp["new_en"] = kNewEn;
    imp["category"] = "mistranslation";
    imp["severity"] = "major";
    imp["error_span"] = "鍵 is under the chair";
    imp["fix_span"] = "key ... is on top of the desk";
    imp["confidence"] = 0.96;
    imp["reason"] = "Fix leaked kanji and mistranslation";

    boost::json::object ckpt;
    ckpt["game_dir"] = wide_to_utf8(game_dir.wstring());
    ckpt["model"] = "gemini-4-argon";
    ckpt["mode"] = "hotspot";
    ckpt["total_entries"] = kSyntheticEntryCount;
    ckpt["improvements_count"] = 1;
    ckpt["checked_offsets"] = boost::json::array{0};
    ckpt["ranked_hotspots"] = boost::json::array{};
    ckpt["improvements"] = boost::json::array{imp};

    const auto ckpt_path = sandbox.write_bytes(
        "EncryptedNovel/script_output/argon_spot_check.json",
        boost::json::serialize(ckpt));

    const std::string decrypt_bat =
        "@echo off\r\n"
        "setlocal\r\n"
        "set \"TARGET_DIR=%~dp0\"\r\n"
        ":parse_decrypt_args\r\n"
        "if \"%~1\"==\"\" goto do_decrypt\r\n"
        "if /I \"%~1\"==\"--dir\" (\r\n"
        "    set \"TARGET_DIR=%~2\"\r\n"
        "    shift\r\n"
        "    shift\r\n"
        "    goto parse_decrypt_args\r\n"
        ")\r\n"
        "shift\r\n"
        "goto parse_decrypt_args\r\n"
        ":do_decrypt\r\n"
        "copy /Y \"%TARGET_DIR%\\script_output\\translated_text.json.enc\" "
        "\"%TARGET_DIR%\\script_output\\translated_text.json\" >nul || exit /b 1\r\n"
        "exit /b 0\r\n";

    const std::string encrypt_bat =
        "@echo off\r\n"
        "setlocal\r\n"
        "set \"TARGET_DIR=%~dp0\"\r\n"
        ":parse_encrypt_args\r\n"
        "if \"%~1\"==\"\" goto do_encrypt\r\n"
        "if /I \"%~1\"==\"--dir\" (\r\n"
        "    set \"TARGET_DIR=%~2\"\r\n"
        "    shift\r\n"
        "    shift\r\n"
        "    goto parse_encrypt_args\r\n"
        ")\r\n"
        "shift\r\n"
        "goto parse_encrypt_args\r\n"
        ":do_encrypt\r\n"
        "copy /Y \"%TARGET_DIR%\\script_output\\translated_text.json\" "
        "\"%TARGET_DIR%\\script_output\\translated_text.json.enc\" >nul || exit /b 1\r\n"
        "exit /b 0\r\n";

    sandbox.write_bytes("decrypt_translations.bat", decrypt_bat);
    sandbox.write_bytes("encrypt_translations.bat", encrypt_bat);

    // First `--apply` run: decrypts `.enc`, updates primary + duplicate JP + dual keys + cache, re-encrypts `.enc`.
    const auto run1 = run_process_utf8(
        exe,
        {"--game-dir", wide_to_utf8(game_dir.wstring()), "--apply", "--out",
         wide_to_utf8(ckpt_path.wstring())},
        sandbox.path(),
        kCliTimeoutMs);

    ASSERT_FALSE(run1.launch_failed) << run1.launch_error;
    ASSERT_FALSE(run1.timed_out);
    EXPECT_EQ(run1.exit_code, 0) << "stderr: " << run1.stderr_text;
    EXPECT_TRUE(run1.stderr_text.empty()) << run1.stderr_text;
    EXPECT_FALSE(run1.stdout_text.empty());

    const auto tt_path = game_dir / "script_output" / "translated_text.json";
    ASSERT_TRUE(std::filesystem::is_regular_file(tt_path));
    const std::string updated_tt_raw = TempSandbox::read_bytes(tt_path);
    // CRLF + 1-space indentation preserved.
    EXPECT_NE(updated_tt_raw.find("\r\n \"scene_01\":"), std::string::npos);

    const auto parsed_tt = boost::json::parse(updated_tt_raw).as_object();
    const auto& s01_arr =
        parsed_tt.at("scene_01").as_object().at("strings").as_array();
    ASSERT_EQ(s01_arr.size(), kScene01RowCount);
    const auto& s01_row = s01_arr[6].as_object();
    EXPECT_EQ(s01_row.at("translated").as_string(), kNewEn);
    EXPECT_EQ(s01_row.at("translation").as_string(), kNewEn);

    const auto& s02_arr =
        parsed_tt.at("scene_02").as_object().at("lines").as_array();
    ASSERT_EQ(s02_arr.size(), kScene02RowCount);
    const auto& s02_row = s02_arr[3].as_object();
    EXPECT_EQ(s02_row.at("translated").as_string(), kNewEn);

    // Re-encrypted `.json.enc` must now match updated plain JSON!
    EXPECT_EQ(TempSandbox::read_bytes(enc_path), updated_tt_raw);

    // Cache key order preserved (`zz_first` < `kDefectJp` < `aa_last`).
    const std::string updated_cache_raw = TempSandbox::read_bytes(cache_path);
    const auto p0 = updated_cache_raw.find("zz_first");
    const auto p1 = updated_cache_raw.find(std::string(kDefectJp));
    const auto p2 = updated_cache_raw.find("aa_last");
    ASSERT_NE(p0, std::string::npos);
    ASSERT_NE(p1, std::string::npos);
    ASSERT_NE(p2, std::string::npos);
    EXPECT_LT(p0, p1);
    EXPECT_LT(p1, p2);
    EXPECT_EQ(boost::json::parse(updated_cache_raw)
                  .as_object()
                  .at(kDefectJp)
                  .as_string(),
              kNewEn);

    // Second `--apply` run: must exit 0 with byte-for-byte idempotent files.
    const auto run2 = run_process_utf8(
        exe,
        {"--game-dir", wide_to_utf8(game_dir.wstring()), "--apply", "--out",
         wide_to_utf8(ckpt_path.wstring())},
        sandbox.path(),
        kCliTimeoutMs);
    ASSERT_FALSE(run2.launch_failed);
    ASSERT_FALSE(run2.timed_out);
    EXPECT_EQ(run2.exit_code, 0) << "stderr: " << run2.stderr_text;
    EXPECT_TRUE(run2.stderr_text.empty());
    EXPECT_EQ(TempSandbox::read_bytes(tt_path), updated_tt_raw);
    EXPECT_EQ(TempSandbox::read_bytes(cache_path), updated_cache_raw);
}

TEST(SpotCheckCliSystemTest,
     OfflineZeroWindowAndCleanPolicyFlagsExecuteWithZeroNetworkCalls) {
    const auto exe = locate_installed_spot_check_exe();
    ASSERT_TRUE(std::filesystem::is_regular_file(exe));

    TempSandbox sandbox("st_cli_policy_zero_net");
    const auto game_dir = sandbox.path() / "CleanPolicyNovel";
    sandbox.write_bytes("CleanPolicyNovel/TONE_OF_NOVEL.md",
                        load_colocated_tone_markdown());
    const std::string clean_tt = build_multi_schema_corpus_crlf(false);
    const auto tt_path = sandbox.write_bytes(
        "CleanPolicyNovel/script_output/translated_text.json", clean_tt);

    // Sentinel dummy key ensures CI without `~/.gemini/.env` succeeds when 0 calls
    // are made AND acts as a hard tripwire if any unexpected network call fires.
    const std::vector<std::pair<std::string, std::string>> env_sentinel{
        {"GEMINI_API_KEY", "offline_sentinel_tripwire_key"}};

    // 1. Clean novel with `--dry-run`, `--lock-non-suspects`, `--include-h-scenes`,
    //    `--fix-punctuation`, `--min-confidence`, `--radar-calls`, `--window`, `--max-windows`.
    const auto dry_out = sandbox.path() / "dry_clean_out.json";
    const auto dry_run = run_process_utf8(
        exe,
        {"--game-dir", wide_to_utf8(game_dir.wstring()),
         "--dry-run",
         "--model", "gemini-4-neon",
         "--thinking-level", "low",
         "--lock-non-suspects",
         "--include-h-scenes",
         "--fix-punctuation",
         "--min-confidence", "0.90",
         "--radar-calls", "0",
         "--window", "8",
         "--max-windows", "2",
         "--out", wide_to_utf8(dry_out.wstring())},
        sandbox.path(),
        kCliTimeoutMs,
        env_sentinel);

    ASSERT_FALSE(dry_run.launch_failed) << dry_run.launch_error;
    ASSERT_FALSE(dry_run.timed_out);
    EXPECT_EQ(dry_run.exit_code, 0) << "stderr: " << dry_run.stderr_text;
    EXPECT_TRUE(dry_run.stderr_text.empty()) << dry_run.stderr_text;
    EXPECT_EQ(TempSandbox::read_bytes(tt_path), clean_tt);
    ASSERT_TRUE(std::filesystem::is_regular_file(dry_out));
    const auto dry_obj =
        boost::json::parse(TempSandbox::read_bytes(dry_out)).as_object();
    EXPECT_EQ(dry_obj.at("model").as_string(), "gemini-4-neon");
    EXPECT_EQ(dry_obj.at("hotspots_detected").to_number<std::size_t>(), 0U);
    EXPECT_EQ(dry_obj.at("windows_checked").to_number<std::size_t>(), 0U);

    // 2. `--mode linear --offset 100 --max-windows 1` (offset past corpus end)
    // exercises linear mode & offset CLI flags with 0 windows checked and 0 calls.
    const auto linear_out = sandbox.path() / "linear_zero_out.json";
    const auto linear_run = run_process_utf8(
        exe,
        {"--game-dir", wide_to_utf8(game_dir.wstring()),
         "--mode", "linear",
         "--offset", std::to_string(kPastEndLinearOffset),
         "--max-windows", "1",
         "--out", wide_to_utf8(linear_out.wstring())},
        sandbox.path(),
        kCliTimeoutMs,
        env_sentinel);

    ASSERT_FALSE(linear_run.launch_failed) << linear_run.launch_error;
    ASSERT_FALSE(linear_run.timed_out);
    EXPECT_EQ(linear_run.exit_code, 0) << "stderr: " << linear_run.stderr_text;
    EXPECT_TRUE(linear_run.stderr_text.empty()) << linear_run.stderr_text;
    ASSERT_TRUE(std::filesystem::is_regular_file(linear_out));
    const auto linear_obj =
        boost::json::parse(TempSandbox::read_bytes(linear_out)).as_object();
    EXPECT_EQ(linear_obj.at("mode").as_string(), "linear");
    EXPECT_EQ(linear_obj.at("windows_checked").to_number<std::size_t>(), 0U);
}

TEST(SpotCheckCliSystemTest,
     DisallowedArgonModelFlagFailsWithExitCodeOneNamingArgonAndZeroSideEffects) {
    const auto exe = locate_installed_spot_check_exe();
    ASSERT_TRUE(std::filesystem::is_regular_file(exe));

    TempSandbox sandbox("st_cli_disallowed_argon");
    const auto game_dir = sandbox.path() / "ArgonRejectedNovel";
    const std::string initial_tt = build_multi_schema_corpus_crlf(true);
    const auto tt_path = sandbox.write_bytes(
        "ArgonRejectedNovel/script_output/translated_text.json", initial_tt);
    const auto pre_files = snapshot_relative_files(game_dir);
    const std::string game_dir_u8 = wide_to_utf8(game_dir.wstring());

    const std::vector<std::pair<std::string, std::string>> env_sentinel{
        {"GEMINI_API_KEY", "offline_sentinel_tripwire_key"}};

    struct DisallowedCliCase {
        const char* name;
        std::vector<std::string> args;
    };
    const std::vector<DisallowedCliCase> disallowed_cases{
        {"default_live_mode_gemini_4_argon",
         {"--game-dir", game_dir_u8, "--model", "gemini-4-argon"}},
        {"scan_only_uppercase_argon_variant",
         {"--game-dir", game_dir_u8, "--scan-only", "--model",
          "GEMINI-4-ARGON-ALT"}},
        {"apply_mode_explicit_argon_model",
         {"--game-dir", game_dir_u8, "--apply", "--model", "gemini-4-argon"}},
    };

    for (const auto& tc : disallowed_cases) {
        SCOPED_TRACE(tc.name);
        const auto res = run_process_utf8(
            exe, tc.args, sandbox.path(), kCliTimeoutMs, env_sentinel);
        ASSERT_FALSE(res.launch_failed) << res.launch_error;
        ASSERT_FALSE(res.timed_out);
        EXPECT_EQ(res.exit_code, 1)
            << "stdout:\n" << res.stdout_text << "\nstderr:\n" << res.stderr_text;
        EXPECT_NE(to_lower_ascii(res.stderr_text).find("argon"),
                  std::string::npos)
            << "Expected stderr to name disallowed argon policy, got:\n"
            << res.stderr_text;
        EXPECT_EQ(snapshot_relative_files(game_dir), pre_files);
        EXPECT_EQ(TempSandbox::read_bytes(tt_path), initial_tt);
    }
}

}  // namespace
}  // namespace spot_checker
