// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Tier-2 `spot_checker`-specific live `gemini-4-carbon` pre-flight gate.
// Verifies `ArgonClient` (`vn::llm_client` delegation) with `json_mode = true`,
// structured Stage 1 Packed GEMBA-MQM Radar JSON triage, Stage 2 Surgical
// Window Repair at the production 40-line window size, VN engine control token
// preservation (`%N`, `$&`), `thought_tokens > 0`, latency within the 600s read
// timeout, and 0-call checkpoint resume idempotency.

#include "client/argon_client.h"
#include "common/utf8_text.h"
#include "common/win32_cli_env.h"
#include "domain/spot_check_types.h"
#include "ports/llm_transport.h"
#include "st_process_harness.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace spot_checker {
namespace {

using test_support::TempSandbox;
using test_support::locate_installed_spot_check_exe;
using test_support::run_process_utf8;

constexpr std::int64_t kMaxExpectedThoughtTokens = 32768;
constexpr DWORD kMillisecondsPerSecond = 1000U;
constexpr DWORD kLiveProcessTimeoutMs =
    static_cast<DWORD>(vn::llm::kDefaultReadTimeoutSeconds) *
    kMillisecondsPerSecond;
constexpr double kMaxProductionWindowLatencySeconds =
    static_cast<double>(vn::llm::kDefaultReadTimeoutSeconds);
constexpr std::size_t kCleanBaselineCount = 12;
constexpr std::size_t kRunawayRepeatCount = 45;
constexpr std::size_t kCompressedRunawayMaxBytes = 120;
constexpr std::size_t kTotalLiveNovelEntries = 90;
constexpr std::size_t kNonTranslatableLiveEntries = 2;
constexpr std::size_t kSecondaryHotspotRowIndex = 80;

constexpr int kEditableDefectPromptId = 1;
constexpr int kCleanControlPromptId = 2;

constexpr std::size_t kUntouchedCleanSlot = 0;
constexpr std::size_t kSilencePassthroughSlot = 1;
constexpr std::size_t kAssetTokenSlot = 2;
constexpr std::size_t kDollarAmpKeySlot = 3;
constexpr std::size_t kPctNLineSlot = 4;
constexpr std::size_t kRunawayScreamSlot = 5;
constexpr std::size_t kCallbackSetupSlot = 6;
constexpr std::size_t kCallbackPayoffSlot = 7;

constexpr std::size_t kMinExpectedHotspots = 2;
constexpr std::size_t kExpectedRadarCalls = 1;
constexpr std::size_t kMinTriagedPassages = 2;
constexpr std::size_t kExpectedWindowsChecked = 1;
constexpr std::size_t kMinExpectedImprovements = 4;
constexpr std::size_t kExpectedLiveApiCalls = 2;
constexpr std::size_t kExpectedSeededCacheEntries = 6;

struct CleanRow {
    std::string_view speaker;
    std::string_view jp;
    std::string_view en;
};

constexpr std::array<CleanRow, kCleanBaselineCount> kCleanBaseline{{
    {"桜庭",
     "「今、何時だ？」",
     "\"What time is it right now?\""},
    {"友貴",
     "「わからないよ」",
     "\"I have no idea.\""},
    {"",
     "あの光の先に、オレが夢見た世界があるはずなのに。",
     "Beyond that light, the world I dreamed of is supposed to be waiting for me."},
    {"",
     "どうしてこの長いトンネルを抜けられない。",
     "Why can't I ever make it out of this endless tunnel?"},
    {"",
     "歩いているのか、立ち止まっているのか、走っているのか。",
     "Am I walking, standing still, or running flat out?"},
    {"",
     "そこには、隣町の景色が広がっていた。",
     "Right beyond it, the scenery of the neighboring town stretched out before me."},
    {"",
     "再び雑誌に目を戻し、パックのコーヒー牛乳をすすった。",
     "He dropped his gaze back to his magazine and took a sip from his carton of coffee milk."},
    {"",
     "窓際にずらりと並ぶ、様々なモニターと機材を乗せた長テーブルを見やる。",
     "I glanced over at the long table by the window, lined end to end with monitors and broadcasting gear."},
    {"太一",
     "「あれ、機材の配線外れてない？」",
     "\"Huh? Aren't some of the cables on that equipment unplugged?\""},
    {"友貴",
     "「新しいマシン組むから、ちょっと構成変える」",
     "\"I'm putting a new machine together, so I'm rearranging the setup a bit.\""},
    {"",
     "電源があるのをいいことに、小型冷蔵庫を置いたりゲーム機を常備したり携帯充電器が一列に並んでいたりと校内でも屈指の無法空間となっている。",
     "Taking full advantage of the power outlets, we had a mini-fridge, game consoles on standby, and a whole row of phone chargers, making it one of the most lawless zones in the school."},
    {"",
     "視線を移した瞬間、放送室の扉が閉まった。",
     "The instant I looked away, the door to the broadcasting room clicked shut."},
}};

std::string json_escape(std::string_view raw) {
    return boost::json::serialize(boost::json::value(raw));
}

std::string build_live_tone_guide() {
    return "# CROSS†CHANNEL — Tone & Speaker Glossary\n"
           "<name_reference>\n"
           "| Japanese | Canonical English |\n"
           "|---|---|\n"
           "| 太一 | Taichi |\n"
           "| 友貴 | Tomoki |\n"
           "| 桜庭 | Sakuraba |\n"
           "| 見里 | Misato |\n"
           "</name_reference>\n"
           "<terminology>\n"
           "| Japanese | Canonical English |\n"
           "|---|---|\n"
           "| 群青学院 | Gunjou Institute |\n"
           "</terminology>\n";
}

std::string make_live_runaway_scream() {
    std::string s = "\"Uwaaaaaah! Stay back!";
    for (std::size_t i = 0; i < kRunawayRepeatCount; ++i) {
        s += " Stay back!";
    }
    s.push_back('"');
    return s;
}

std::string require_live_api_key_or_skip_ci() {
    const std::string api_key = ArgonClient::resolve_gemini_api_key();
    if (!api_key.empty()) {
        return api_key;
    }
    const char* ci_env = std::getenv("GITHUB_ACTIONS");
    if (ci_env != nullptr && std::string_view(ci_env) == "true") {
        return {};
    }
    ADD_FAILURE()
        << "GEMINI_API_KEY is missing locally; LIVE_API system tests must fail "
           "loudly rather than silently skipping.";
    return {};
}

TEST(SpotCheckLiveApiSystemTest,
     ArgonClientJsonModeAdapterPreservesEngineTokensWithActiveThinking) {
    const std::string api_key = require_live_api_key_or_skip_ci();
    if (api_key.empty()) {
        if (::testing::Test::HasFailure()) {
            return;
        }
        GTEST_SKIP() << "GEMINI_API_KEY absent on GitHub Actions runner";
    }

    ArgonClient client(api_key, std::string(kDefaultModel));

    LlmRequest req;
    req.system_prompt =
        "You are a precision Japanese-to-English visual novel editor. "
        "Return ONLY a JSON array of objects with fields: id (integer), "
        "new_en (string), category (string), severity (string), "
        "confidence (number in [0,1]), reason (string). "
        "Preserve inline VN engine control tokens such as %N and $& verbatim. "
        "Only emit entries that have a genuine translation error.";
    req.user_prompt =
        "[1] [EDITABLE SUSPECT]\n"
        "JP: 「$&群青学院の屋上にある鍵は%N机の上にあるぞ」\n"
        "EN: \"$&The Gunjou Institute rooftop key is%Nunder the chair.\"\n\n"
        "[2] [EDITABLE SUSPECT]\n"
        "JP: 「ありがとう、助かったよ」\n"
        "EN: \"Thanks, that really helps.\"\n";
    req.thinking_level = std::string(kDefaultThinkingLevel);
    req.max_output_tokens = kDefaultMaxOutputTokens;
    req.json_mode = true;

    const auto res = client.generate(req);
    ASSERT_TRUE(res.is_ok()) << "ArgonClient TLS call failed: "
                             << res.error_message()
                             << " (http_status=" << res.status().http_status
                             << ")";

    const auto& llm = res.value();
    std::cout << "[LIVE_API] ArgonClient JSON adapter: prompt_tokens="
              << llm.prompt_tokens << " output_tokens=" << llm.output_tokens
              << " thought_tokens=" << llm.thought_tokens << "\n";
    EXPECT_FALSE(llm.content_blocked);
    EXPECT_EQ(llm.finish_reason, "STOP");
    EXPECT_GT(llm.prompt_tokens, 0);
    EXPECT_GT(llm.output_tokens, 0);
    EXPECT_GT(llm.thought_tokens, 0);
    EXPECT_LE(llm.thought_tokens, kMaxExpectedThoughtTokens);

    const auto parsed = boost::json::parse(llm.text);
    ASSERT_TRUE(parsed.is_array()) << "Raw response: " << llm.text;
    const auto& arr = parsed.as_array();
    ASSERT_FALSE(arr.empty()) << "Expected line 1 mistranslation to be flagged";

    bool found_line_one_fix = false;
    for (const auto& item_val : arr) {
        ASSERT_TRUE(item_val.is_object());
        const auto& item = item_val.as_object();
        const auto id = item.at("id").to_number<int>();
        EXPECT_NE(id, kCleanControlPromptId)
            << "Clean line 2 should not be rewritten";
        if (id == kEditableDefectPromptId) {
            found_line_one_fix = true;
            const std::string new_en(item.at("new_en").as_string());
            const std::string lower_en = to_lower_ascii(new_en);
            EXPECT_NE(new_en.find("$&"), std::string::npos) << new_en;
            EXPECT_NE(new_en.find("%N"), std::string::npos) << new_en;
            EXPECT_NE(lower_en.find("gunjou institute"), std::string::npos)
                << new_en;
            EXPECT_TRUE(lower_en.find("desk") != std::string::npos ||
                        lower_en.find("table") != std::string::npos)
                << new_en;
            EXPECT_EQ(lower_en.find("chair"), std::string::npos) << new_en;
        }
    }
    EXPECT_TRUE(found_line_one_fix);
}

TEST(SpotCheckLiveApiSystemTest,
     PackagedBinaryLiveEndToEndRadarSurgicalRepairEngineTokensAndResume) {
    const std::string api_key = require_live_api_key_or_skip_ci();
    if (api_key.empty()) {
        if (::testing::Test::HasFailure()) {
            return;
        }
        GTEST_SKIP() << "GEMINI_API_KEY absent on GitHub Actions runner";
    }

    const auto exe = locate_installed_spot_check_exe();
    ASSERT_TRUE(std::filesystem::is_regular_file(exe))
        << "Missing staged packaged binary at: " << wide_to_utf8(exe.wstring());

    TempSandbox sandbox("st_live_e2e_spot_checker");
    const auto game_dir = sandbox.path() / "LiveSpotCheckNovel";
    sandbox.write_bytes("LiveSpotCheckNovel/TONE_OF_NOVEL.md",
                        build_live_tone_guide());

    struct NovelEntry {
        std::string speaker;
        std::string jp;
        std::string en;
    };

    std::vector<NovelEntry> rows;
    rows.reserve(kTotalLiveNovelEntries);
    for (std::size_t i = 0; i < kTotalLiveNovelEntries; ++i) {
        const auto& base = kCleanBaseline[i % kCleanBaseline.size()];
        rows.push_back(
            {std::string(base.speaker), std::string(base.jp), std::string(base.en)});
    }

    constexpr std::string_view kUntouchedCleanJp = "「今、何時だ？」";
    constexpr std::string_view kUntouchedCleanEn =
        "\"What time is it right now?\"";
    constexpr std::string_view kSilencePassthrough = "「……」";
    constexpr std::string_view kAssetToken = "_bg_clubroom_day";

    constexpr std::string_view kDollarAmpKeyJp =
        "「$&群青学院の図書室にある鍵は机の上にあるぞ」";
    constexpr std::string_view kDollarAmpKeyOldEn =
        "\"$&The Gunjou Institute library 鍵 is under the chair.\"";

    constexpr std::string_view kPctNLineJp =
        "「第一位『謎』、%N第二位『衝撃』という結果になったぞ」";
    constexpr std::string_view kPctNLineOldEn =
        "\"First place was 'Mystery',%Nand second place was 衝撃 in the rankings.\"";

    constexpr std::string_view kRunawayJp = "「うわあああああっ！」";
    const std::string runaway_old_en = make_live_runaway_scream();
    constexpr std::string_view kCallbackSetupJp =
        "「今夜は屋上で流星群を観測する約束だったよな？」";
    constexpr std::string_view kCallbackSetupEn =
        "\"We promised to observe the meteor shower on the rooftop tonight, right?\"";
    constexpr std::string_view kCallbackPayoffJp =
        "「ああ、流星群を見るために望遠鏡まで持ってきたぞ」";
    constexpr std::string_view kCallbackPayoffOldEn =
        "\"Yeah, I even brought a telescope to watch the fireworks大会.\"";

    rows[kUntouchedCleanSlot] = {"桜庭", std::string(kUntouchedCleanJp),
                                 std::string(kUntouchedCleanEn)};
    rows[kSilencePassthroughSlot] = {"友貴", std::string(kSilencePassthrough),
                                     std::string(kSilencePassthrough)};
    rows[kAssetTokenSlot] = {"", std::string(kAssetToken),
                             std::string(kAssetToken)};
    rows[kDollarAmpKeySlot] = {"太一", std::string(kDollarAmpKeyJp),
                               std::string(kDollarAmpKeyOldEn)};
    rows[kPctNLineSlot] = {"太一", std::string(kPctNLineJp),
                           std::string(kPctNLineOldEn)};
    rows[kRunawayScreamSlot] = {"太一", std::string(kRunawayJp), runaway_old_en};
    rows[kCallbackSetupSlot] = {"太一", std::string(kCallbackSetupJp),
                                std::string(kCallbackSetupEn)};
    rows[kCallbackPayoffSlot] = {"友貴", std::string(kCallbackPayoffJp),
                                 std::string(kCallbackPayoffOldEn)};

    rows[kSecondaryHotspotRowIndex] = {
        "友貴",
        "「うちのクラスは自習になったんだ」",
        "\"Our class got turned into 自習 study hall.\""};

    std::ostringstream tt_json;
    tt_json << "{\n  \"sn.bin\": {\n    \"strings\": [\n";
    for (std::size_t i = 0; i < rows.size(); ++i) {
        tt_json << "      {\n";
        if (rows[i].speaker.empty()) {
            tt_json << "        \"speaker\": null,\n";
        } else {
            tt_json << "        \"speaker\": " << json_escape(rows[i].speaker)
                    << ",\n";
        }
        tt_json << "        \"text\": " << json_escape(rows[i].jp) << ",\n"
                << "        \"translated\": " << json_escape(rows[i].en) << "\n"
                << "      }" << (i + 1U < rows.size() ? "," : "") << "\n";
    }
    tt_json << "    ]\n  }\n}\n";

    const auto tt_path = sandbox.write_bytes(
        "LiveSpotCheckNovel/script_output/translated_text.json", tt_json.str());

    std::ostringstream cache_json;
    cache_json << "{\n"
               << "  \"zz_first_sentinel\": \"Untouched first\",\n"
               << "  " << json_escape(kDollarAmpKeyJp) << ": "
               << json_escape(kDollarAmpKeyOldEn) << ",\n"
               << "  \"mm_middle_sentinel\": \"Untouched middle\",\n"
               << "  " << json_escape(kPctNLineJp) << ": "
               << json_escape(kPctNLineOldEn) << ",\n"
               << "  " << json_escape(kRunawayJp) << ": "
               << json_escape(runaway_old_en) << ",\n"
               << "  \"aa_last_sentinel\": \"Untouched last\"\n"
               << "}\n";
    const auto cache_path = sandbox.write_bytes(
        "LiveSpotCheckNovel/script_output/translation_cache_anthropic.json",
        cache_json.str());

    const auto out_ckpt_path =
        game_dir / "script_output" / "argon_spot_check.json";
    const std::vector<std::string> cli_args{
        "--game-dir", wide_to_utf8(game_dir.wstring()),
        "--radar-calls", std::to_string(kExpectedRadarCalls),
        "--max-windows", std::to_string(kExpectedWindowsChecked),
        "--out", wide_to_utf8(out_ckpt_path.wstring())};

    const auto t0 = std::chrono::steady_clock::now();
    const auto run1 =
        run_process_utf8(exe, cli_args, sandbox.path(), kLiveProcessTimeoutMs);
    const double pass1_latency_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
            .count();

    ASSERT_FALSE(run1.launch_failed) << run1.launch_error;
    ASSERT_FALSE(run1.timed_out);
    ASSERT_EQ(run1.exit_code, 0)
        << "stderr:\n" << run1.stderr_text << "\nstdout:\n" << run1.stdout_text;
    EXPECT_TRUE(run1.stderr_text.empty()) << run1.stderr_text;
    EXPECT_LT(pass1_latency_s, kMaxProductionWindowLatencySeconds);

    ASSERT_TRUE(std::filesystem::is_regular_file(out_ckpt_path));
    const auto ckpt1 =
        boost::json::parse(TempSandbox::read_bytes(out_ckpt_path)).as_object();

    EXPECT_EQ(ckpt1.at("total_entries").to_number<std::size_t>(),
              kTotalLiveNovelEntries - kNonTranslatableLiveEntries);
    EXPECT_TRUE(ckpt1.at("ols_valid").as_bool());
    EXPECT_GE(ckpt1.at("hotspots_detected").to_number<std::size_t>(),
              kMinExpectedHotspots);
    EXPECT_EQ(ckpt1.at("radar_calls_made").to_number<std::size_t>(),
              kExpectedRadarCalls);
    EXPECT_GE(ckpt1.at("radar_passages_triaged").to_number<std::size_t>(),
              kMinTriagedPassages);
    EXPECT_EQ(ckpt1.at("windows_checked").to_number<std::size_t>(),
              kExpectedWindowsChecked);
    EXPECT_EQ(ckpt1.at("lines_checked").to_number<std::size_t>(),
              kDefaultWindowSize);
    EXPECT_GE(ckpt1.at("improvements_count").to_number<std::size_t>(),
              kMinExpectedImprovements);

    const auto& usage1 = ckpt1.at("token_usage").as_object();
    const auto e2e_prompt_tokens =
        usage1.at("prompt_tokens").to_number<std::int64_t>();
    const auto e2e_output_tokens =
        usage1.at("output_tokens").to_number<std::int64_t>();
    const auto e2e_thought_tokens =
        usage1.at("thought_tokens").to_number<std::int64_t>();
    std::cout << "[LIVE_API] Packaged CLI E2E Pass 1 (" << pass1_latency_s
              << "s): api_calls="
              << usage1.at("api_calls").to_number<std::size_t>()
              << " prompt_tokens=" << e2e_prompt_tokens
              << " output_tokens=" << e2e_output_tokens
              << " thought_tokens=" << e2e_thought_tokens << "\n";
    EXPECT_EQ(usage1.at("api_calls").to_number<std::size_t>(),
              kExpectedLiveApiCalls);
    EXPECT_GT(e2e_prompt_tokens, 0);
    EXPECT_GT(e2e_output_tokens, 0);
    EXPECT_GT(e2e_thought_tokens, 0);

    const std::string updated_tt_raw = TempSandbox::read_bytes(tt_path);
    const auto updated_tt = boost::json::parse(updated_tt_raw).as_object();
    const auto& strings =
        updated_tt.at("sn.bin").as_object().at("strings").as_array();
    ASSERT_EQ(strings.size(), kTotalLiveNovelEntries);

    EXPECT_EQ(
        strings[kUntouchedCleanSlot].as_object().at("translated").as_string(),
        kUntouchedCleanEn);
    EXPECT_EQ(strings[kSilencePassthroughSlot]
                  .as_object()
                  .at("translated")
                  .as_string(),
              kSilencePassthrough);
    EXPECT_EQ(strings[kAssetTokenSlot].as_object().at("translated").as_string(),
              kAssetToken);
    EXPECT_EQ(
        strings[kCallbackSetupSlot].as_object().at("translated").as_string(),
        kCallbackSetupEn);

    const std::string fixed_dollar_amp_line = std::string(
        strings[kDollarAmpKeySlot].as_object().at("translated").as_string());
    EXPECT_FALSE(has_japanese(fixed_dollar_amp_line)) << fixed_dollar_amp_line;
    EXPECT_FALSE(has_cjk_or_fullwidth_punctuation(fixed_dollar_amp_line))
        << fixed_dollar_amp_line;
    EXPECT_NE(fixed_dollar_amp_line.find("$&"), std::string::npos)
        << fixed_dollar_amp_line;
    EXPECT_NE(to_lower_ascii(fixed_dollar_amp_line).find("gunjou institute"),
              std::string::npos)
        << fixed_dollar_amp_line;
    EXPECT_EQ(to_lower_ascii(fixed_dollar_amp_line).find("chair"),
              std::string::npos)
        << fixed_dollar_amp_line;

    const std::string fixed_pct_n_line = std::string(
        strings[kPctNLineSlot].as_object().at("translated").as_string());
    EXPECT_FALSE(has_japanese(fixed_pct_n_line)) << fixed_pct_n_line;
    EXPECT_FALSE(has_cjk_or_fullwidth_punctuation(fixed_pct_n_line))
        << fixed_pct_n_line;
    EXPECT_NE(fixed_pct_n_line.find("%N"), std::string::npos)
        << fixed_pct_n_line;

    const std::string fixed_runaway_line = std::string(
        strings[kRunawayScreamSlot].as_object().at("translated").as_string());
    EXPECT_LT(fixed_runaway_line.size(), kCompressedRunawayMaxBytes)
        << fixed_runaway_line;
    EXPECT_FALSE(has_japanese(fixed_runaway_line)) << fixed_runaway_line;

    const std::string fixed_callback_line = std::string(
        strings[kCallbackPayoffSlot].as_object().at("translated").as_string());
    EXPECT_FALSE(has_japanese(fixed_callback_line)) << fixed_callback_line;
    EXPECT_NE(to_lower_ascii(fixed_callback_line).find("meteor"),
              std::string::npos)
        << fixed_callback_line;

    const std::string updated_cache_raw = TempSandbox::read_bytes(cache_path);
    const auto p0 = updated_cache_raw.find("zz_first_sentinel");
    const auto p1 = updated_cache_raw.find(std::string(kDollarAmpKeyJp));
    const auto p2 = updated_cache_raw.find("mm_middle_sentinel");
    const auto p3 = updated_cache_raw.find(std::string(kPctNLineJp));
    const auto p4 = updated_cache_raw.find(std::string(kRunawayJp));
    const auto p5 = updated_cache_raw.find("aa_last_sentinel");
    ASSERT_NE(p0, std::string::npos);
    ASSERT_NE(p1, std::string::npos);
    ASSERT_NE(p2, std::string::npos);
    ASSERT_NE(p3, std::string::npos);
    ASSERT_NE(p4, std::string::npos);
    ASSERT_NE(p5, std::string::npos);
    EXPECT_LT(p0, p1);
    EXPECT_LT(p1, p2);
    EXPECT_LT(p2, p3);
    EXPECT_LT(p3, p4);
    EXPECT_LT(p4, p5);

    const auto parsed_cache = boost::json::parse(updated_cache_raw).as_object();
    EXPECT_EQ(parsed_cache.size(), kExpectedSeededCacheEntries);
    EXPECT_EQ(parsed_cache.at(kDollarAmpKeyJp).as_string(),
              fixed_dollar_amp_line);
    EXPECT_EQ(parsed_cache.at(kPctNLineJp).as_string(), fixed_pct_n_line);
    EXPECT_EQ(parsed_cache.at(kRunawayJp).as_string(), fixed_runaway_line);

    const auto run2 = run_process_utf8(
        exe,
        cli_args,
        sandbox.path(),
        kLiveProcessTimeoutMs,
        {{"GEMINI_API_KEY", "resume_zero_api_call_tripwire_key"}});
    ASSERT_FALSE(run2.launch_failed) << run2.launch_error;
    ASSERT_FALSE(run2.timed_out);
    ASSERT_EQ(run2.exit_code, 0)
        << "stderr:\n" << run2.stderr_text << "\nstdout:\n" << run2.stdout_text;
    EXPECT_TRUE(run2.stderr_text.empty()) << run2.stderr_text;

    const auto ckpt2 =
        boost::json::parse(TempSandbox::read_bytes(out_ckpt_path)).as_object();
    EXPECT_EQ(ckpt2.at("token_usage")
                  .as_object()
                  .at("api_calls")
                  .to_number<std::size_t>(),
              kExpectedLiveApiCalls);
    EXPECT_EQ(TempSandbox::read_bytes(tt_path), updated_tt_raw);
    EXPECT_EQ(TempSandbox::read_bytes(cache_path), updated_cache_raw);
}

}  // namespace
}  // namespace spot_checker
