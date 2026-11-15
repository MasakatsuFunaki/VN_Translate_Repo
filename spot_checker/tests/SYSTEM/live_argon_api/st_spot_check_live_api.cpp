// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "client/argon_client.h"
#include "common/utf8_text.h"
#include "common/win32_cli_env.h"
#include "domain/spot_check_types.h"
#include "ports/llm_transport.h"
#include "st_process_harness.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace spot_checker {
namespace {

using test_support::TempSandbox;
using test_support::locate_installed_spot_check_exe;
using test_support::run_process_utf8;

constexpr int kDirectWireMaxOutputTokens = 1024;
constexpr std::int64_t kMaxExpectedThoughtTokens = 2560;
constexpr DWORD kLiveProcessTimeoutMs = 180000;
constexpr std::size_t kRunawayRepeatCount = 45;
constexpr std::size_t kCompressedRunawayMaxBytes = 120;
constexpr std::size_t kTotalLiveNovelEntries = 46;
constexpr std::size_t kNonTranslatableLiveEntries = 2;

struct CleanRow {
    std::string_view speaker;
    std::string_view jp;
    std::string_view en;
};

constexpr std::array<CleanRow, 12> kCleanBaseline{{
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

std::string json_escape(std::string_view raw) {
    return boost::json::serialize(boost::json::value(raw));
}

std::string load_colocated_live_tone_guide() {
    const auto path = executable_path().parent_path() / "live_tone_of_novel.md";
    const std::string bytes = TempSandbox::read_bytes(path);
    if (!bytes.empty()) {
        return bytes;
    }
    return "# Tone Guide\n<name_reference>\n| 蒼井 遥 | Haruka Aoi (she/her) |\n"
           "| 高瀬 蓮 | Ren Takase (he/him) |\n</name_reference>\n"
           "<terminology>\n| 星見学園 | Hoshimi Academy |\n</terminology>\n";
}

std::string make_live_runaway_scream() {
    std::string s = "\"Kyaaaah! Stay back!";
    for (std::size_t i = 0; i < kRunawayRepeatCount; ++i) {
        s += " Stay back!";
    }
    s.push_back('"');
    return s;
}

TEST(SpotCheckLiveApiSystemTest, ArgonClientDirectTlsWireAndThinkingTelemetry) {
    const std::string api_key = ArgonClient::resolve_gemini_api_key();
    ASSERT_FALSE(api_key.empty())
        << "GEMINI_API_KEY is missing from environment and ~/.gemini/.env; "
           "LIVE_API system tests must fail loudly rather than skipping.";

    ArgonClient client(api_key, std::string(kDefaultArgonModel));

    LlmRequest req;
    req.system_prompt =
        "You are a precision Japanese-to-English visual novel editor. "
        "Return ONLY a JSON array of objects with fields: id (integer), "
        "new_en (string), category (string), severity (string), "
        "confidence (number in [0,1]), reason (string). "
        "Only emit entries that have a genuine translation error.";
    req.user_prompt =
        "[1] [EDITABLE SUSPECT]\n"
        "JP: 「星見学園の図書室にある鍵は机の上にありますよ」\n"
        "EN: \"The Hoshimi Academy library key is under the chair.\"\n\n"
        "[2] [EDITABLE SUSPECT]\n"
        "JP: 「ありがとう、助かったよ」\n"
        "EN: \"Thanks, that really helps.\"\n";
    req.thinking_budget = kDefaultThinkingBudgetTokens;
    req.max_output_tokens = kDirectWireMaxOutputTokens;
    req.json_mode = true;

    const auto res = client.generate(req);
    ASSERT_TRUE(res.is_ok()) << "ArgonClient TLS call failed: "
                             << res.error_message()
                             << " (http_status=" << res.status().http_status
                             << ")";

    const auto& llm = res.value();
    EXPECT_EQ(llm.finish_reason, "STOP");
    EXPECT_GT(llm.prompt_tokens, 0);
    EXPECT_GT(llm.output_tokens, 0);
    EXPECT_GE(llm.thought_tokens, 0);
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
        // Clean line 2 must not be modified.
        EXPECT_NE(id, 2) << "Clean line 2 should not be rewritten";
        if (id == 1) {
            found_line_one_fix = true;
            const std::string new_en =
                std::string(item.at("new_en").as_string());
            const std::string lower_en = to_lower_ascii(new_en);
            EXPECT_NE(lower_en.find("hoshimi academy"), std::string::npos)
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
     PackagedBinaryLiveEndToEndRadarAndSurgicalRepair) {
    const std::string api_key = ArgonClient::resolve_gemini_api_key();
    ASSERT_FALSE(api_key.empty())
        << "GEMINI_API_KEY is missing from environment and ~/.gemini/.env";

    const auto exe = locate_installed_spot_check_exe();
    ASSERT_TRUE(std::filesystem::is_regular_file(exe))
        << "Missing staged packaged binary at: " << wide_to_utf8(exe.wstring());

    TempSandbox sandbox("st_live_e2e");
    const auto game_dir = sandbox.path() / "LiveSyntheticNovel";
    sandbox.write_bytes("LiveSyntheticNovel/TONE_OF_NOVEL.md",
                        load_colocated_live_tone_guide());

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

    constexpr std::string_view kUntouchedCleanEn =
        "\"Good morning, the weather looks clear again today.\"";
    constexpr std::string_view kSilencePassthrough = "「……」";
    constexpr std::string_view kAssetToken = "_bg_rooftop_evening";
    constexpr std::string_view kLeakedKeyJp =
        "「星見学園の図書室にある鍵は机の上にありますよ」";
    constexpr std::string_view kLeakedKeyOldEn =
        "\"The Hoshimi Academy library 鍵 is under the chair.\"";
    constexpr std::string_view kRunawayJp =
        "「きゃあああっ！来ないでっ！」";
    const std::string runaway_old_en = make_live_runaway_scream();

    // Window A (`[0, 10)`): 3 untouched controls + 5 unmistakable defects.
    rows[0] = {"蒼井 遥", "「おはよう、今日もいい天気だね」",
               std::string(kUntouchedCleanEn)};
    rows[1] = {"蒼井 遥", std::string(kSilencePassthrough),
               std::string(kSilencePassthrough)};
    rows[2] = {"", std::string(kAssetToken), std::string(kAssetToken)};
    rows[3] = {"蒼井 遥", std::string(kLeakedKeyJp),
               std::string(kLeakedKeyOldEn)};
    rows[4] = {"高瀬 蓮", "「まだ帰らないでくれ、大事な話があるんだ」",
               "\"Please go home right now, I have nothing important to say.\""};
    rows[5] = {"高瀬 蓮", "「蒼井さんは図書室で本を読んでいたよ」",
               "\"Haruka Aoi was reading a 本 in the library; he looked busy.\""};
    rows[6] = {"蒼井 遥", std::string(kRunawayJp), runaway_old_en};
    rows[7] = {"高瀬 蓮", "「今夜は屋上で流星群を観測する約束だったよな？」",
               "\"We promised to observe the meteor shower on the rooftop tonight, right?\""};
    rows[8] = {"蒼井 遥", "「ええ、流星群を見るために望遠鏡まで持ってきたわ」",
               "\"Yes, I even brought a telescope to watch the fireworks大会.\""};

    // Secondary minor defect far away (`index 40`) so `hotspots_detected >= 2`
    // and Stage 1 Radar packs both candidate windows into 1 Radar call (2 live calls total).
    rows[40] = {"高瀬 蓮", "「帰り道に駅前の書店へ寄っていこう」",
                "\"Let's stop by the station 書店 on the way back.\""};

    std::ostringstream tt_json;
    tt_json << "{\n  \"scene_01\": {\n    \"strings\": [\n";
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
        "LiveSyntheticNovel/script_output/translated_text.json", tt_json.str());

    std::ostringstream cache_json;
    cache_json << "{\n"
               << "  \"zz_first_sentinel\": \"Untouched first\",\n"
               << "  " << json_escape(kLeakedKeyJp) << ": "
               << json_escape(kLeakedKeyOldEn) << ",\n"
               << "  \"mm_middle_sentinel\": \"Untouched middle\",\n"
               << "  " << json_escape(kRunawayJp) << ": "
               << json_escape(runaway_old_en) << ",\n"
               << "  \"aa_last_sentinel\": \"Untouched last\"\n"
               << "}\n";
    const auto cache_path = sandbox.write_bytes(
        "LiveSyntheticNovel/script_output/translation_cache_anthropic.json",
        cache_json.str());

    const auto out_ckpt_path =
        game_dir / "script_output" / "argon_spot_check.json";

    // Strictly bounded to `<= 2` live API calls (`--radar-calls 1 --max-windows 1`).
    const auto run = run_process_utf8(
        exe,
        {"--game-dir", wide_to_utf8(game_dir.wstring()),
         "--radar-calls", "1",
         "--max-windows", "1",
         "--window", "10",
         "--out", wide_to_utf8(out_ckpt_path.wstring())},
        sandbox.path(),
        kLiveProcessTimeoutMs);

    ASSERT_FALSE(run.launch_failed) << run.launch_error;
    ASSERT_FALSE(run.timed_out);
    ASSERT_EQ(run.exit_code, 0)
        << "stderr:\n" << run.stderr_text << "\nstdout:\n" << run.stdout_text;
    EXPECT_TRUE(run.stderr_text.empty()) << run.stderr_text;

    ASSERT_TRUE(std::filesystem::is_regular_file(out_ckpt_path));
    const auto ckpt =
        boost::json::parse(TempSandbox::read_bytes(out_ckpt_path)).as_object();

    EXPECT_EQ(ckpt.at("total_entries").to_number<std::size_t>(),
              kTotalLiveNovelEntries - kNonTranslatableLiveEntries);
    EXPECT_TRUE(ckpt.at("ols_valid").as_bool());
    EXPECT_GE(ckpt.at("hotspots_detected").to_number<std::size_t>(), 2U);
    EXPECT_EQ(ckpt.at("radar_calls_made").to_number<std::size_t>(), 1U);
    EXPECT_EQ(ckpt.at("windows_checked").to_number<std::size_t>(), 1U);
    EXPECT_GE(ckpt.at("improvements_count").to_number<std::size_t>(), 3U);

    const auto& usage = ckpt.at("token_usage").as_object();
    EXPECT_EQ(usage.at("api_calls").to_number<std::size_t>(), 2U);
    EXPECT_GT(usage.at("prompt_tokens").to_number<std::int64_t>(), 0);
    EXPECT_GT(usage.at("output_tokens").to_number<std::int64_t>(), 0);
    EXPECT_GE(usage.at("thought_tokens").to_number<std::int64_t>(), 0);

    // Verify `translated_text.json` preserved clean/passthrough/asset entries and repaired defects.
    const auto updated_tt =
        boost::json::parse(TempSandbox::read_bytes(tt_path)).as_object();
    const auto& strings =
        updated_tt.at("scene_01").as_object().at("strings").as_array();
    ASSERT_EQ(strings.size(), kTotalLiveNovelEntries);

    EXPECT_EQ(strings[0].as_object().at("translated").as_string(),
              kUntouchedCleanEn);
    EXPECT_EQ(strings[1].as_object().at("translated").as_string(),
              kSilencePassthrough);
    EXPECT_EQ(strings[2].as_object().at("translated").as_string(), kAssetToken);

    const std::string fixed_key_line =
        std::string(strings[3].as_object().at("translated").as_string());
    EXPECT_FALSE(has_japanese(fixed_key_line)) << fixed_key_line;
    EXPECT_EQ(to_lower_ascii(fixed_key_line).find("chair"), std::string::npos)
        << fixed_key_line;

    const std::string fixed_runaway_line =
        std::string(strings[6].as_object().at("translated").as_string());
    EXPECT_LT(fixed_runaway_line.size(), kCompressedRunawayMaxBytes)
        << fixed_runaway_line;
    EXPECT_FALSE(has_japanese(fixed_runaway_line)) << fixed_runaway_line;

    // Verify `translation_cache_anthropic.json` preserved insertion order and updated keys.
    const std::string updated_cache_raw = TempSandbox::read_bytes(cache_path);
    const auto p0 = updated_cache_raw.find("zz_first_sentinel");
    const auto p1 = updated_cache_raw.find(std::string(kLeakedKeyJp));
    const auto p2 = updated_cache_raw.find("mm_middle_sentinel");
    const auto p3 = updated_cache_raw.find(std::string(kRunawayJp));
    const auto p4 = updated_cache_raw.find("aa_last_sentinel");
    ASSERT_NE(p0, std::string::npos);
    ASSERT_NE(p1, std::string::npos);
    ASSERT_NE(p2, std::string::npos);
    ASSERT_NE(p3, std::string::npos);
    ASSERT_NE(p4, std::string::npos);
    EXPECT_LT(p0, p1);
    EXPECT_LT(p1, p2);
    EXPECT_LT(p2, p3);
    EXPECT_LT(p3, p4);

    const auto parsed_cache = boost::json::parse(updated_cache_raw).as_object();
    EXPECT_EQ(parsed_cache.size(), 5U);
    EXPECT_EQ(parsed_cache.at(kLeakedKeyJp).as_string(), fixed_key_line);
    EXPECT_EQ(parsed_cache.at(kRunawayJp).as_string(), fixed_runaway_line);
}

}  // namespace
}  // namespace spot_checker
