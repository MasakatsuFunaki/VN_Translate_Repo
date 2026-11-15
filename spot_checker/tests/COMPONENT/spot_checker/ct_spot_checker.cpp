// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "checker/spot_checker.h"

#include "common/status.h"
#include "common/utf8_text.h"
#include "common/win32_cli_env.h"
#include "domain/spot_check_types.h"
#include "ct_test_fakes.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace spot_checker {
namespace {

using test_support::CapturingLogSink;
using test_support::FakeClock;
using test_support::FaultInjectingFileStore;
using test_support::RecordingCryptSync;
using test_support::ScriptedLlmTransport;
using test_support::TempSandbox;
using test_support::find_radar_passage_id_for_jp;
using test_support::inspect_target_in_user_prompt;

constexpr std::size_t kSmallWindowSize = 10;
constexpr std::size_t kCompactWindowSize = 6;
constexpr std::size_t kTinyPairWindowSize = 2;
constexpr std::size_t kTwentySixWindows = 26;
constexpr std::size_t kDefaultSyncEvery = 25;
constexpr std::size_t kRunawayRepeatCount = 38;
constexpr std::size_t kRadarScene01Count = 60;
constexpr std::size_t kRadarScene02Count = 36;
constexpr std::size_t kScene02SpeakerDefectIndex = 5;
constexpr std::size_t kHClusterStartIndex = 16;
constexpr std::size_t kKnobsScene01Count = 28;
constexpr std::size_t kKnobsSustainedHStartIndex = 8;
constexpr std::size_t kContagionCorpusSize = 120;
constexpr std::size_t kMinContagionSpans = 2;
constexpr std::size_t kContagionLeadIndex = 21;
constexpr std::size_t kContagionMidIndex = 25;
constexpr std::size_t kContagionPeakIndex = 28;
constexpr std::size_t kContagionEdgeIndex = 29;
constexpr std::size_t kContagionSpillIndex = 35;
constexpr std::size_t kContagionTailIndex = 95;
constexpr std::size_t kContagionMaxWindows = 6;
constexpr std::size_t kContagionExpectedImprovements = 6;
constexpr double kContagionZThreshold = 1.0;
constexpr std::size_t kDryRunScene01Count = 10;
constexpr std::size_t kDryRunScene02Count = 6;
constexpr double kStrictMinConfidence = 0.88;
constexpr double kElevatedZThreshold = 50.0;
constexpr std::int64_t kRadarPromptTokens = 410;
constexpr std::int64_t kRadarOutputTokens = 65;
constexpr std::int64_t kRadarThoughtTokens = 190;
constexpr std::int64_t kWindowPromptTokens = 520;
constexpr std::int64_t kWindowOutputTokens = 95;
constexpr std::int64_t kWindowThoughtTokens = 240;
constexpr std::size_t kBlockedCorpusSize = 80;
constexpr std::size_t kBlockedHardTriggerIdx = 0;
constexpr std::size_t kBlockedLeftCompanionIdx = 2;
constexpr std::size_t kBlockedRightSalvagedIdx = 7;
constexpr std::size_t kBlockedContextPoisonIdx = 36;
constexpr std::size_t kBlockedContextRetryTargetIdx = 45;
constexpr std::size_t kBlockedContextBefore = 10;
constexpr std::size_t kBlockedMaxWindows = 10;
constexpr std::size_t kMinRun1BlockedApiCalls = 6;
constexpr std::size_t kExpectedBlockedWarnings = 2;
constexpr std::size_t kExpectedRun1Improvements = 2;
constexpr std::size_t kExpectedTotalBlockedImprovements = 4;
constexpr double kBlockedZThreshold = 0.2;
constexpr double kBlockedMajorConfidence = 0.96;
constexpr double kBlockedCriticalConfidence = 0.97;
constexpr std::int64_t kBlockedRadarPromptTokens = 310;
constexpr std::int64_t kBlockedRadarOutputTokens = 0;
constexpr std::int64_t kBlockedRadarThoughtTokens = 45;
constexpr std::int64_t kBlockedWinPromptTokens = 420;
constexpr std::int64_t kBlockedWinOutputTokens = 0;
constexpr std::int64_t kBlockedWinThoughtTokens = 55;
constexpr std::int64_t kNormalWinPromptTokens = 460;
constexpr std::int64_t kNormalWinOutputTokens = 70;
constexpr std::int64_t kNormalWinThoughtTokens = 130;

constexpr std::size_t kSustainedCtScene01Lines = 92;
constexpr std::size_t kSustainedCtScene02Lines = 24;
constexpr std::size_t kSustainedCtOpenDefectA = 2;
constexpr std::size_t kSustainedCtOpenDefectB = 5;
constexpr std::size_t kSustainedCtOpenDefectEdge = 9;
constexpr std::size_t kSustainedCtVocalHBegin = 10;
constexpr std::size_t kSustainedCtVocalHEnd = 36;
constexpr std::size_t kSustainedCtVocalDefectIdx = 14;
constexpr std::size_t kSustainedCtBanterBegin = 44;
constexpr std::size_t kSustainedCtBanterDefectIdx = 45;
constexpr std::size_t kSustainedCtCombatBegin = 64;
constexpr std::size_t kSustainedCtCombatDefectIdx = 76;
constexpr std::size_t kSustainedCtLiteraryLocalDefectIdx = 10;
constexpr std::size_t kSustainedCtLinearOffset = 5;
constexpr std::size_t kSustainedCtLinearMaxWindows = 3;
constexpr std::size_t kSustainedCtExpectedTrimmedLines = 5;
constexpr std::size_t kSustainedCtMinSkippedLinearHWindows = 2;
constexpr std::size_t kSustainedCtExpectedDefaultImprovements = 5;
constexpr std::size_t kSustainedCtExpectedOptInImprovements = 7;

struct BaselinePair {
    std::string_view speaker_jp;
    std::string_view jp;
    std::string_view en;
};

constexpr std::array<BaselinePair, 12> kCleanBaselinePool{{
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

std::string load_colocated_tone_guide() {
    const auto colocated = executable_path().parent_path() / "tone_of_novel.md";
    const std::string text = TempSandbox::read_bytes(colocated);
    if (!text.empty()) {
        return text;
    }
    return "# Tone Guide\n<name_reference>\n| 蒼井 遥 | Haruka Aoi |\n"
           "| 高瀬 蓮 | Ren Takase |\n</name_reference>\n"
           "<terminology>\n| 星見学園 | Hoshimi Academy |\n"
           "| 天文同好会 | Astronomy Club |\n</terminology>\n"
           "<formatting_rules>\nBATCH_TRANSLATOR_ONLY_SENTINEL\n</formatting_rules>\n";
}

std::string json_escape(std::string_view raw) {
    return boost::json::serialize(boost::json::value(raw));
}

std::string make_runaway_scream() {
    std::string out = "\"Kyaaaah! Stay back!";
    for (std::size_t i = 0; i < kRunawayRepeatCount; ++i) {
        out += " Stay back!";
    }
    out.push_back('"');
    return out;
}

bool contains_log_substring(const std::vector<std::string>& messages,
                            std::string_view needle) {
    return std::any_of(messages.begin(),
                       messages.end(),
                       [needle](const std::string& msg) {
                           return msg.find(needle) != std::string::npos;
                       });
}

bool has_any_tmp_files(const std::filesystem::path& dir) {
    constexpr std::string_view kTmpSuffix = ".tmp";
    std::error_code ec;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(dir, ec)) {
        const auto name = wide_to_utf8(entry.path().filename().wstring());
        if (name.size() >= kTmpSuffix.size() &&
            name.substr(name.size() - kTmpSuffix.size()) == kTmpSuffix) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> list_recursive_relative_files(
    const std::filesystem::path& root) {
    std::vector<std::string> files;
    std::error_code ec;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(root, ec)) {
        if (entry.is_regular_file(ec)) {
            files.push_back(
                wide_to_utf8(std::filesystem::relative(entry.path(), root, ec)
                                 .generic_wstring()));
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::string build_candidate_item_json(int id,
                                      std::string_view new_en,
                                      std::string_view category,
                                      std::string_view severity,
                                      double confidence,
                                      std::string_view reason,
                                      std::string_view error_span = {},
                                      std::string_view fix_span = {}) {
    boost::json::object obj;
    obj["id"] = id;
    obj["new_en"] = new_en;
    obj["category"] = category;
    obj["severity"] = severity;
    obj["confidence"] = confidence;
    obj["reason"] = reason;
    if (!error_span.empty()) {
        obj["error_span"] = error_span;
    }
    if (!fix_span.empty()) {
        obj["fix_span"] = fix_span;
    }
    return boost::json::serialize(obj);
}

struct SyntheticRow {
    std::string speaker;
    std::string jp;
    std::string en;
    bool dual_translation_key = false;
};

// Formats a multi-script `translated_text.json` with explicit 1-space indentation
// and `\r\n` CRLF line endings so format-preservation assertions are exact.
std::string serialize_crlf_indent1_corpus(
    const std::vector<SyntheticRow>& scene01_strings,
    const std::vector<SyntheticRow>& scene02_lines) {
    std::ostringstream oss;
    oss << "{\r\n";
    oss << " \"scene_01\": {\r\n";
    oss << "  \"strings\": [\r\n";
    for (std::size_t i = 0; i < scene01_strings.size(); ++i) {
        const auto& r = scene01_strings[i];
        oss << "   {\r\n";
        if (r.speaker.empty()) {
            oss << "    \"speaker\": null,\r\n";
        } else {
            oss << "    \"speaker\": " << json_escape(r.speaker) << ",\r\n";
        }
        oss << "    \"text\": " << json_escape(r.jp) << ",\r\n";
        oss << "    \"translated\": " << json_escape(r.en);
        if (r.dual_translation_key) {
            oss << ",\r\n    \"translation\": " << json_escape(r.en) << "\r\n";
        } else {
            oss << "\r\n";
        }
        oss << "   }" << (i + 1U < scene01_strings.size() ? "," : "") << "\r\n";
    }
    oss << "  ]\r\n";
    oss << " },\r\n";
    oss << " \"scene_02\": {\r\n";
    oss << "  \"lines\": [\r\n";
    for (std::size_t i = 0; i < scene02_lines.size(); ++i) {
        const auto& r = scene02_lines[i];
        oss << "   {\r\n";
        oss << "    \"content\": " << json_escape(r.jp) << ",\r\n";
        oss << "    \"translated\": " << json_escape(r.en) << "\r\n";
        oss << "   }" << (i + 1U < scene02_lines.size() ? "," : "") << "\r\n";
    }
    oss << "  ]\r\n";
    oss << " }\r\n";
    oss << "}\r\n";
    return oss.str();
}

SpotCheckerDependencies bind_deps(ScriptedLlmTransport& transport,
                                  FaultInjectingFileStore& store,
                                  RecordingCryptSync& crypt,
                                  FakeClock& clock,
                                  CapturingLogSink& logs) {
    SpotCheckerDependencies deps;
    deps.transport = &transport;
    deps.file_store = &store;
    deps.crypt_sync = &crypt;
    deps.clock = &clock;
    deps.log_sink = &logs;
    return deps;
}

TEST(SpotCheckerComponentTest,
     ScanOnlyComputesOlsAndHotspotsWithoutApiCallsOrGameDirWrites) {
    TempSandbox sandbox("ct_scan_only");
    ScriptedLlmTransport transport;
    FaultInjectingFileStore store;
    RecordingCryptSync crypt;
    FakeClock clock;
    CapturingLogSink logs;
    const auto deps = bind_deps(transport, store, crypt, clock, logs);

    // Precondition & repository error gates through SpotChecker::create.
    SpotCheckConfig empty_cfg;
    const auto missing_dir_res = SpotChecker::create(empty_cfg, deps);
    EXPECT_FALSE(missing_dir_res.is_ok());

    const auto game_dir = sandbox.path() / "SyntheticNovel";
    sandbox.write_bytes("SyntheticNovel/TONE_OF_NOVEL.md",
                        load_colocated_tone_guide());
    sandbox.write_bytes("SyntheticNovel/script_output/translated_text.json",
                        "{ broken json ");

    SpotCheckConfig bad_json_cfg;
    bad_json_cfg.game_dir = wide_to_utf8(game_dir.wstring());
    bad_json_cfg.scan_only = true;
    const auto bad_json_res = SpotChecker::create(bad_json_cfg, deps);
    ASSERT_FALSE(bad_json_res.is_ok());
    EXPECT_EQ(bad_json_res.error_code(), ErrorCode::CorruptJson);

    crypt.set_fail_decrypt(true);
    const auto decrypt_fail_res = SpotChecker::create(bad_json_cfg, deps);
    ASSERT_FALSE(decrypt_fail_res.is_ok());
    EXPECT_EQ(decrypt_fail_res.error_code(), ErrorCode::CryptSyncFailed);
    crypt.set_fail_decrypt(false);

    // Build a 36-entry novel with 1 clear hotspot cluster around index 15.
    std::vector<SyntheticRow> scene01;
    std::vector<SyntheticRow> scene02;
    for (std::size_t i = 0; i < 24U; ++i) {
        const auto& base = kCleanBaselinePool[i % kCleanBaselinePool.size()];
        scene01.push_back(
            {std::string(base.speaker_jp), std::string(base.jp), std::string(base.en), false});
    }
    scene01[15] = {
        "蒼井 遥",
        "「星見学園の図書室にある鍵は机の上にありますよ」",
        "\"The Hoshimi Academy library 鍵 is under the chair.\"",
        true};
    scene01[16] = {
        "高瀬 蓮",
        "「きゃあああっ！離してくれっ！」",
        make_runaway_scream(),
        false};

    for (std::size_t i = 0; i < 12U; ++i) {
        const auto& base = kCleanBaselinePool[i % kCleanBaselinePool.size()];
        scene02.push_back(
            {"", std::string(base.jp), std::string(base.en), false});
    }

    sandbox.write_bytes("SyntheticNovel/script_output/translated_text.json",
                        serialize_crlf_indent1_corpus(scene01, scene02));
    const auto before_files = list_recursive_relative_files(game_dir);

    // 1. Scan-only with empty `out_path`: zero disk writes anywhere, zero LLM calls.
    SpotCheckConfig scan_mem_cfg;
    scan_mem_cfg.game_dir = wide_to_utf8(game_dir.wstring());
    scan_mem_cfg.scan_only = true;
    scan_mem_cfg.window_size = kSmallWindowSize;

    auto checker_res = SpotChecker::create(scan_mem_cfg, deps);
    ASSERT_TRUE(checker_res.is_ok()) << checker_res.error_message();
    auto checker = std::move(checker_res).value();

    EXPECT_EQ(checker.entries().size(), 36U);
    EXPECT_NE(checker.system_prompt().find("Haruka Aoi"), std::string::npos);
    EXPECT_NE(checker.system_prompt().find("Hoshimi Academy"), std::string::npos);
    EXPECT_EQ(checker.system_prompt().find("BATCH_TRANSLATOR_ONLY_SENTINEL"),
              std::string::npos);

    const auto mem_report_res = checker.run();
    ASSERT_TRUE(mem_report_res.is_ok()) << mem_report_res.error_message();
    const auto& mem_report = mem_report_res.value();

    EXPECT_EQ(mem_report.total_entries, 36U);
    EXPECT_TRUE(mem_report.ols_valid);
    EXPECT_GE(mem_report.ols_slope, 0.6);
    EXPECT_LE(mem_report.ols_slope, 3.0);
    EXPECT_GE(mem_report.hotspots_detected, 1U);
    EXPECT_EQ(mem_report.radar_calls_made, 0U);
    EXPECT_EQ(mem_report.windows_checked, 0U);
    EXPECT_EQ(mem_report.token_usage.api_calls, 0U);
    EXPECT_TRUE(transport.requests().empty());
    EXPECT_EQ(store.write_attempts(), 0U);
    EXPECT_EQ(list_recursive_relative_files(game_dir), before_files);

    // 2. Scan-only with explicit `out_path` outside `game_dir`.
    const auto external_report_path = sandbox.path() / "reports" / "scan_out.json";
    SpotCheckConfig scan_out_cfg = scan_mem_cfg;
    scan_out_cfg.out_path = wide_to_utf8(external_report_path.wstring());

    auto checker_out_res = SpotChecker::create(scan_out_cfg, deps);
    ASSERT_TRUE(checker_out_res.is_ok()) << checker_out_res.error_message();
    const auto out_report_res = checker_out_res.value().run();
    ASSERT_TRUE(out_report_res.is_ok()) << out_report_res.error_message();

    EXPECT_TRUE(std::filesystem::exists(external_report_path));
    EXPECT_EQ(list_recursive_relative_files(game_dir), before_files);
    const auto parsed_scan =
        boost::json::parse(TempSandbox::read_bytes(external_report_path)).as_object();
    EXPECT_EQ(parsed_scan.at("total_entries").to_number<std::size_t>(), 36U);
    EXPECT_GE(parsed_scan.at("hotspots_detected").to_number<std::size_t>(), 1U);

    // 3. High `z_threshold` filters out all candidate windows cleanly.
    SpotCheckConfig high_z_cfg = scan_mem_cfg;
    high_z_cfg.z_threshold = kElevatedZThreshold;
    auto high_z_checker = SpotChecker::create(high_z_cfg, deps);
    ASSERT_TRUE(high_z_checker.is_ok());
    const auto high_z_report = high_z_checker.value().run();
    ASSERT_TRUE(high_z_report.is_ok());
    EXPECT_EQ(high_z_report.value().hotspots_detected, 0U);
    EXPECT_TRUE(high_z_report.value().ranked_hotspots.empty());
}

TEST(SpotCheckerComponentTest,
     TwoStageRadarReRanksWindowsEnforcesValidatorGatesAndPreservesArtifactStyles) {
    TempSandbox sandbox("ct_radar_repair");
    ScriptedLlmTransport transport;
    FaultInjectingFileStore store;
    RecordingCryptSync crypt;
    FakeClock clock;
    CapturingLogSink logs;
    const auto deps = bind_deps(transport, store, crypt, clock, logs);

    const auto game_dir = sandbox.path() / "NovelRadar";
    sandbox.write_bytes("NovelRadar/TONE_OF_NOVEL.md",
                        load_colocated_tone_guide());

    // Build an 84-entry multi-schema corpus:
    // - Window region A (around index 6): multiple defects + poisoned candidates testbed.
    // - Window region B (around index 44): severe defects + 1 isolated H-scene line.
    // - Window region C (indices 74..83): pure explicit H-scene window skipped by skip_h_scenes.
    std::vector<SyntheticRow> scene01;
    for (std::size_t i = 0; i < 60U; ++i) {
        const auto& base = kCleanBaselinePool[i % kCleanBaselinePool.size()];
        scene01.push_back(
            {std::string(base.speaker_jp), std::string(base.jp), std::string(base.en), false});
    }

    constexpr std::string_view kWinAGoodJp =
        "「%sさん、あと%d分で星見学園の$str20が閉まりますよ」";
    constexpr std::string_view kWinAGoodOldEn =
        "\"%s, in %d minutes Hoshimi Academy's $str20 closes 鍵.\"";
    constexpr std::string_view kWinAGoodNewEn =
        "\"%s, in %d minutes Hoshimi Academy's $str20 will close.\"";

    constexpr std::string_view kWinADropTokenJp =
        "「受付番号%dの荷物を倉庫から運んでください」";
    constexpr std::string_view kWinADropTokenOldEn =
        "\"Please carry parcel #%d from the 倉庫.\"";

    constexpr std::string_view kWinADropGlossaryJp =
        "「放課後は星見学園の屋上で待ち合わせよう」";
    constexpr std::string_view kWinADropGlossaryOldEn =
        "\"Let's meet on the Hoshimi Academy roof 鍵 after school.\"";

    constexpr std::string_view kWinAUnchangedSpanJp =
        "「古い望遠鏡のレンズを丁寧に磨いておいたよ」";
    constexpr std::string_view kWinAUnchangedSpanOldEn =
        "\"I polished the broken lens 鍵 carefully.\"";

    constexpr std::string_view kWinAPunctOnlyJp =
        "「駅の売店で温かい缶コーヒーを二本買ってきたよ」";
    constexpr std::string_view kWinAPunctOnlyOldEn =
        "\"I bought two warm cans of coffee at the station kiosk.\"";

    constexpr std::string_view kWinALowConfJp =
        "「明日の部活は雨なら中止になるらしい」";
    constexpr std::string_view kWinALowConfOldEn =
        "\"Tomorrow's club practice will be 鍵 canceled if it rains.\"";

    scene01[4] = {"蒼井 遥", std::string(kWinAGoodJp),
                  std::string(kWinAGoodOldEn), true};
    scene01[5] = {"高瀬 蓮", std::string(kWinADropTokenJp),
                  std::string(kWinADropTokenOldEn), false};
    scene01[6] = {"蒼井 遥", std::string(kWinADropGlossaryJp),
                  std::string(kWinADropGlossaryOldEn), false};
    scene01[7] = {"高瀬 蓮", std::string(kWinAUnchangedSpanJp),
                  std::string(kWinAUnchangedSpanOldEn), false};
    scene01[8] = {"蒼井 遥", std::string(kWinAPunctOnlyJp),
                  std::string(kWinAPunctOnlyOldEn), false};
    scene01[9] = {"高瀬 蓮", std::string(kWinALowConfJp),
                  std::string(kWinALowConfOldEn), false};

    constexpr std::string_view kWinBRunawayJp =
        "「きゃあああっ！誰か助けてっ！」";
    const std::string win_b_runaway_old_en = make_runaway_scream();
    constexpr std::string_view kWinBRunawayNewEn =
        "\"Kyaaaah! Somebody help me!\"";

    constexpr std::string_view kWinBDirtyBanterJp =
        "「朝からエロ本だの変態だの大声で騒ぐなよ」";
    constexpr std::string_view kWinBDirtyBanterOldEn =
        "\"Quit yelling about porn mags and perverts first thing in the morning.\"";

    scene01[43] = {"高瀬 蓮", std::string(kWinBDirtyBanterJp),
                   std::string(kWinBDirtyBanterOldEn), false};
    scene01[44] = {"蒼井 遥", std::string(kWinBRunawayJp),
                   win_b_runaway_old_en, false};

    std::vector<SyntheticRow> scene02;
    for (std::size_t i = 0; i < kRadarScene02Count; ++i) {
        const auto& base = kCleanBaselinePool[i % kCleanBaselinePool.size()];
        scene02.push_back(
            {"", std::string(base.jp), std::string(base.en), false});
    }

    constexpr std::string_view kScene02SpeakerLineJp =
        "高瀬 蓮\r\n「天文同好会の部室は三階の奥にあるんだ」";
    constexpr std::string_view kScene02SpeakerLineOldEn =
        "Ren Takase\r\n\"The Astronomy Club room is in the basement 鍵.\"";
    constexpr std::string_view kScene02SpeakerLineEchoedNewEn =
        "[Ren Takase] \"The Astronomy Club room is at the back of the third floor.\"";
    constexpr std::string_view kScene02SpeakerLineRestoredEn =
        "Ren Takase\r\n\"The Astronomy Club room is at the back of the third floor.\"";

    scene02[kScene02SpeakerDefectIndex] = {
        "", std::string(kScene02SpeakerLineJp),
        std::string(kScene02SpeakerLineOldEn), false};

    // 20-line sustained H-scene stretch at the tail of `scene_02` (`indices 16..35`,
    // `global_index 76..95`) seeded with leaked kanji so `HotspotDetector` would
    // score it heavily if sustained H-scene avoidance did not mask it out.
    constexpr std::string_view kSustainedHMarkerJp =
        "「あぁっ、中に熱い精液が注ぎ込まれていく……♡」";
    for (std::size_t i = kHClusterStartIndex; i < kRadarScene02Count; ++i) {
        scene02[i] = {
            "",
            std::string(kSustainedHMarkerJp),
            "\"Aaah, hot cum is pouring deep inside my pussy 鍵...!\"",
            false};
    }

    sandbox.write_bytes("NovelRadar/script_output/translated_text.json",
                        serialize_crlf_indent1_corpus(scene01, scene02));

    // Insertion-ordered LF 2-space cache with deliberate non-alphabetical key order.
    std::ostringstream cache_json;
    cache_json << "{\n"
               << "  \"zz_untouched_first_key\": \"Untouched sentinel one.\",\n"
               << "  " << json_escape(kWinAGoodJp) << ": "
               << json_escape(kWinAGoodOldEn) << ",\n"
               << "  \"mm_untouched_middle_key\": \"Untouched sentinel two.\",\n"
               << "  " << json_escape(kWinBRunawayJp) << ": "
               << json_escape(win_b_runaway_old_en) << ",\n"
               << "  \"aa_untouched_last_key\": \"Untouched sentinel three.\"\n"
               << "}\n";
    sandbox.write_bytes(
        "NovelRadar/script_output/translation_cache_anthropic.json",
        cache_json.str());

    // Script dynamic outer transport:
    // - Call 0: Stage 1 Radar ranks Window B (`critical=3`, MQM=75) > scene_02 (`critical=1`, MQM=25) > Window A (`major=1`, MQM=5).
    // - Subsequent Stage 2 calls: emit targeted valid + poisoned repairs based on window content.
    transport.set_default_handler(
        [&](const LlmRequest& req, std::size_t call_idx) -> Result<LlmResponse> {
            LlmResponse resp;
            resp.finish_reason = "STOP";

            if (call_idx == 0U) {
                const int pid_a =
                    find_radar_passage_id_for_jp(req.user_prompt, kWinAGoodJp);
                const int pid_b =
                    find_radar_passage_id_for_jp(req.user_prompt, kWinBRunawayJp);
                const int pid_c = find_radar_passage_id_for_jp(
                    req.user_prompt, "天文同好会の部室は三階の奥にあるんだ");

                boost::json::array radar_arr;
                if (pid_a > 0) {
                    radar_arr.push_back(boost::json::object{
                        {"passage_id", pid_a},
                        {"critical_count", 0},
                        {"major_count", 1},
                        {"minor_count", 0},
                        {"suspect_ids", boost::json::array{1, 2}}});
                }
                if (pid_b > 0) {
                    radar_arr.push_back(boost::json::object{
                        {"passage_id", pid_b},
                        {"critical_count", 3},
                        {"major_count", 0},
                        {"minor_count", 0},
                        {"suspect_ids", boost::json::array{1, 2, 3}}});
                }
                if (pid_c > 0) {
                    radar_arr.push_back(boost::json::object{
                        {"passage_id", pid_c},
                        {"critical_count", 1},
                        {"major_count", 0},
                        {"minor_count", 0},
                        {"suspect_ids", boost::json::array{1, 2}}});
                }
                resp.text = boost::json::serialize(radar_arr);
                resp.prompt_tokens = kRadarPromptTokens;
                resp.output_tokens = kRadarOutputTokens;
                resp.thought_tokens = kRadarThoughtTokens;
                return Result<LlmResponse>::success(resp);
            }

            resp.prompt_tokens = kWindowPromptTokens;
            resp.output_tokens = kWindowOutputTokens;
            resp.thought_tokens = kWindowThoughtTokens;

            // Window B: verify isolated H-scene line was redacted, and repair runaway scream.
            const auto b_info =
                inspect_target_in_user_prompt(req.user_prompt, kWinBRunawayJp);
            if (b_info.found) {
                resp.text = "[" +
                            build_candidate_item_json(
                                b_info.target_id,
                                kWinBRunawayNewEn,
                                "artifact_or_note",
                                "critical",
                                0.97,
                                "Compress runaway repetition loop",
                                "Stay back! Stay back!",
                                "Somebody help me!") +
                            "]";
                return Result<LlmResponse>::success(resp);
            }

            // scene_02 window: echoes `[Ren Takase]` prefix to exercise speaker restoration.
            const auto c_info = inspect_target_in_user_prompt(
                req.user_prompt, "天文同好会の部室は三階の奥にあるんだ");
            if (c_info.found) {
                resp.text = "[" +
                            build_candidate_item_json(
                                c_info.target_id,
                                kScene02SpeakerLineEchoedNewEn,
                                "mistranslation",
                                "critical",
                                0.95,
                                "三階の奥 means back of the third floor, not basement",
                                "in the basement 鍵",
                                "at the back of the third floor") +
                            "]";
                return Result<LlmResponse>::success(resp);
            }

            // Window A: emit 1 valid fix + 5 poisoned candidates.
            std::vector<std::string> items;
            const auto good =
                inspect_target_in_user_prompt(req.user_prompt, kWinAGoodJp);
            if (good.found) {
                items.push_back(build_candidate_item_json(
                    good.target_id,
                    kWinAGoodNewEn,
                    "mistranslation",
                    "major",
                    0.94,
                    "Remove leaked kanji while keeping placeholders and glossary",
                    "closes 鍵",
                    "will close"));
            }
            const auto drop_tok = inspect_target_in_user_prompt(
                req.user_prompt, kWinADropTokenJp);
            if (drop_tok.found) {
                // Poisoned: drops `%d` placeholder!
                items.push_back(build_candidate_item_json(
                    drop_tok.target_id,
                    "\"Please carry the parcel from the warehouse.\"",
                    "mistranslation",
                    "major",
                    0.95,
                    "Dropped placeholder"));
            }
            const auto drop_gloss = inspect_target_in_user_prompt(
                req.user_prompt, kWinADropGlossaryJp);
            if (drop_gloss.found) {
                // Poisoned: drops canonical school name `Hoshimi Academy`!
                items.push_back(build_candidate_item_json(
                    drop_gloss.target_id,
                    "\"Let's meet on the school roof after class.\"",
                    "mistranslation",
                    "major",
                    0.95,
                    "Dropped canonical glossary term"));
            }
            const auto unchanged_span = inspect_target_in_user_prompt(
                req.user_prompt, kWinAUnchangedSpanJp);
            if (unchanged_span.found) {
                // Poisoned: `error_span` ("broken lens") still present in `new_en`!
                items.push_back(build_candidate_item_json(
                    unchanged_span.target_id,
                    "\"I polished the broken lens thoroughly.\"",
                    "mistranslation",
                    "major",
                    0.95,
                    "Claimed fix left error_span intact",
                    "broken lens",
                    "telescope lens"));
            }
            const auto punct_only = inspect_target_in_user_prompt(
                req.user_prompt, kWinAPunctOnlyJp);
            if (punct_only.found) {
                // Poisoned: punctuation-only tweak while `fix_punctuation == false`!
                items.push_back(build_candidate_item_json(
                    punct_only.target_id,
                    "\"I bought two warm cans of coffee, at the station kiosk!\"",
                    "stiff_translationese",
                    "minor",
                    0.95,
                    "Punctuation tweak only"));
            }
            const auto low_conf = inspect_target_in_user_prompt(
                req.user_prompt, kWinALowConfJp);
            if (low_conf.found) {
                // Poisoned: confidence 0.72 < kStrictMinConfidence (0.88)!
                items.push_back(build_candidate_item_json(
                    low_conf.target_id,
                    "\"Tomorrow's club practice will be canceled if it rains.\"",
                    "mistranslation",
                    "minor",
                    0.72,
                    "Below confidence floor"));
            }

            std::string json_arr = "[";
            for (std::size_t i = 0; i < items.size(); ++i) {
                if (i > 0U) {
                    json_arr += ",";
                }
                json_arr += items[i];
            }
            json_arr += "]";
            resp.text = std::move(json_arr);
            return Result<LlmResponse>::success(resp);
        });

    SpotCheckConfig cfg;
    cfg.game_dir = wide_to_utf8(game_dir.wstring());
    cfg.mode = SpotCheckMode::Hotspot;
    cfg.window_size = kSmallWindowSize;
    cfg.context_before = 4U;
    cfg.context_after = 4U;
    cfg.radar_calls = 1U;
    cfg.radar_pack_size = 6U;
    cfg.max_windows = 3U;
    cfg.min_confidence = kStrictMinConfidence;
    cfg.skip_h_scenes = true;
    cfg.fix_punctuation = false;
    cfg.auto_apply = true;
    cfg.model = "gemini-4-argon";

    auto checker_res = SpotChecker::create(cfg, deps);
    ASSERT_TRUE(checker_res.is_ok()) << checker_res.error_message();
    const auto run_res = checker_res.value().run();
    ASSERT_TRUE(run_res.is_ok()) << run_res.error_message();
    const auto& report = run_res.value();

    // 1 Radar call + 3 Stage 2 window calls = 4 calls total.
    ASSERT_EQ(transport.requests().size(), 4U);
    EXPECT_EQ(report.radar_calls_made, 1U);
    EXPECT_GE(report.radar_passages_triaged, 3U);
    EXPECT_EQ(report.windows_checked, 3U);

    // Sustained 20-line H-scene (`global_index 76..95`) is completely excluded from
    // `ranked_hotspots` and from every Stage-1 / Stage-2 LLM prompt.
    constexpr std::size_t kSustainedHGlobalStart =
        kRadarScene01Count + kHClusterStartIndex;
    for (const auto& hw : report.ranked_hotspots) {
        EXPECT_LE(hw.offset + hw.length, kSustainedHGlobalStart);
    }
    for (const auto& req : transport.requests()) {
        EXPECT_EQ(req.user_prompt.find(kSustainedHMarkerJp), std::string::npos);
    }

    // Stage 1 Radar MQM re-ranking: Window B (MQM 75) inspected before scene_02 (MQM 25) and Window A (MQM 5).
    // Isolated dirty-banter line (`scene01[43]`) is sent verbatim and NEVER redacted!
    EXPECT_NE(transport.requests()[1].user_prompt.find(kWinBRunawayJp),
              std::string::npos);
    EXPECT_NE(transport.requests()[1].user_prompt.find(kWinBDirtyBanterJp),
              std::string::npos);
    EXPECT_EQ(transport.requests()[1].user_prompt.find("[REDACTED EXPLICIT SCENE LINE]"),
              std::string::npos);
    EXPECT_NE(transport.requests()[2].user_prompt.find("天文同好会の部室は三階の奥にあるんだ"),
              std::string::npos);
    EXPECT_NE(transport.requests()[3].user_prompt.find(kWinAGoodJp),
              std::string::npos);

    // Exact token telemetry aggregation across Radar + 3 Stage 2 windows.
    EXPECT_EQ(report.token_usage.api_calls, 4U);
    EXPECT_EQ(report.token_usage.radar_calls_made, 1U);
    EXPECT_EQ(report.token_usage.repair_calls_made, 3U);
    EXPECT_EQ(report.token_usage.prompt_tokens,
              kRadarPromptTokens + 3 * kWindowPromptTokens);
    EXPECT_EQ(report.token_usage.output_tokens,
              kRadarOutputTokens + 3 * kWindowOutputTokens);
    EXPECT_EQ(report.token_usage.thought_tokens,
              kRadarThoughtTokens + 3 * kWindowThoughtTokens);

    // Exactly 3 valid fixes accepted; all 5 poisoned candidates in Window A rejected!
    EXPECT_EQ(report.improvements_found, 3U);
    EXPECT_GE(report.candidates_rejected_by_validator, 4U);
    EXPECT_EQ(report.script_entries_updated, 3U);
    EXPECT_EQ(report.cache_entries_updated, 2U);

    // Verify CRLF + 1-space indentation and dual-key / Speaker\r\n updates on `translated_text.json`.
    const auto updated_tt_raw = TempSandbox::read_bytes(
        game_dir / "script_output" / "translated_text.json");
    EXPECT_NE(updated_tt_raw.find("\r\n \"scene_01\":"), std::string::npos);

    const auto updated_tt = boost::json::parse(updated_tt_raw).as_object();
    const auto& s01 =
        updated_tt.at("scene_01").as_object().at("strings").as_array();
    ASSERT_EQ(s01.size(), kRadarScene01Count);
    EXPECT_EQ(s01[4].as_object().at("translated").as_string(), kWinAGoodNewEn);
    EXPECT_EQ(s01[4].as_object().at("translation").as_string(), kWinAGoodNewEn);
    // Poisoned lines remain byte-for-byte unchanged!
    EXPECT_EQ(s01[5].as_object().at("translated").as_string(), kWinADropTokenOldEn);
    EXPECT_EQ(s01[6].as_object().at("translated").as_string(), kWinADropGlossaryOldEn);
    EXPECT_EQ(s01[7].as_object().at("translated").as_string(), kWinAUnchangedSpanOldEn);
    EXPECT_EQ(s01[8].as_object().at("translated").as_string(), kWinAPunctOnlyOldEn);
    EXPECT_EQ(s01[9].as_object().at("translated").as_string(), kWinALowConfOldEn);
    // Clean dirty-banter line untouched; runaway scream compressed.
    EXPECT_EQ(s01[43].as_object().at("translated").as_string(),
              kWinBDirtyBanterOldEn);
    EXPECT_EQ(s01[44].as_object().at("translated").as_string(), kWinBRunawayNewEn);

    const auto& s02 =
        updated_tt.at("scene_02").as_object().at("lines").as_array();
    ASSERT_EQ(s02.size(), kRadarScene02Count);
    EXPECT_EQ(s02[kScene02SpeakerDefectIndex].as_object().at("translated").as_string(),
              kScene02SpeakerLineRestoredEn);

    // Verify `translation_cache_anthropic.json` preserves LF, exact key count (5), and insertion order.
    const auto updated_cache_raw = TempSandbox::read_bytes(
        game_dir / "script_output" / "translation_cache_anthropic.json");
    EXPECT_EQ(updated_cache_raw.find("\r\n"), std::string::npos);
    const auto pos_k0 = updated_cache_raw.find("zz_untouched_first_key");
    const auto pos_k1 = updated_cache_raw.find(std::string(kWinAGoodJp));
    const auto pos_k2 = updated_cache_raw.find("mm_untouched_middle_key");
    const auto pos_k3 = updated_cache_raw.find(std::string(kWinBRunawayJp));
    const auto pos_k4 = updated_cache_raw.find("aa_untouched_last_key");
    ASSERT_NE(pos_k0, std::string::npos);
    ASSERT_NE(pos_k1, std::string::npos);
    ASSERT_NE(pos_k2, std::string::npos);
    ASSERT_NE(pos_k3, std::string::npos);
    ASSERT_NE(pos_k4, std::string::npos);
    EXPECT_LT(pos_k0, pos_k1);
    EXPECT_LT(pos_k1, pos_k2);
    EXPECT_LT(pos_k2, pos_k3);
    EXPECT_LT(pos_k3, pos_k4);

    const auto parsed_cache = boost::json::parse(updated_cache_raw).as_object();
    EXPECT_EQ(parsed_cache.size(), 5U);
    EXPECT_EQ(parsed_cache.at(kWinAGoodJp).as_string(), kWinAGoodNewEn);
    EXPECT_EQ(parsed_cache.at(kWinBRunawayJp).as_string(), kWinBRunawayNewEn);
    EXPECT_FALSE(has_any_tmp_files(game_dir));
}

TEST(SpotCheckerComponentTest,
     LockNonSuspectsIncludeHScenesFixPunctuationAndLinearOffsetKnobs) {
    TempSandbox sandbox("ct_policy_knobs");
    ScriptedLlmTransport transport;
    FaultInjectingFileStore store;
    RecordingCryptSync crypt;
    FakeClock clock;
    CapturingLogSink logs;
    const auto deps = bind_deps(transport, store, crypt, clock, logs);

    const auto game_dir = sandbox.path() / "NovelKnobs";
    sandbox.write_bytes("NovelKnobs/TONE_OF_NOVEL.md",
                        load_colocated_tone_guide());

    std::vector<SyntheticRow> scene01;
    for (std::size_t i = 0; i < kKnobsScene01Count; ++i) {
        const auto& base = kCleanBaselinePool[i % kCleanBaselinePool.size()];
        scene01.push_back(
            {std::string(base.speaker_jp), std::string(base.jp), std::string(base.en), false});
    }
    // Make `[8, 28)` a 20-line sustained `（Ｈ）`-tagged scene so `skip_h_scenes = false`
    // (`--include-h-scenes`) genuinely unlocks a sustained H-scene span in Linear mode.
    for (std::size_t i = kKnobsSustainedHStartIndex; i < kKnobsScene01Count; ++i) {
        scene01[i].speaker = "高瀬 蓮（Ｈ）";
    }

    // Defect at index 1 BEFORE linear `offset = 6` — must remain untouched!
    constexpr std::string_view kPreOffsetDefectJp =
        "「序盤の不具合はスキップされるはずだ」";
    constexpr std::string_view kPreOffsetDefectOldEn =
        "\"Early defect 鍵 before offset.\"";
    scene01[1] = {"高瀬 蓮", std::string(kPreOffsetDefectJp),
                  std::string(kPreOffsetDefectOldEn), false};

    // Inside linear window `[6, 12)`:
    // - index 6: clean line far from suspect -> `[READ-ONLY CONTEXT]` under `review_all_in_window = false`!
    // - index 9: explicit H-scene line WITH romaji translator note `(senpai)` and dropped outer quotes!
    // - index 10: immediate `+1` banter neighbor next to index 9 -> unlocked via `+/-1` rule!
    constexpr std::string_view kLockedCleanJp =
        "「七時のバスに乗れば余裕で間に合うよ」";
    constexpr std::string_view kLockedCleanEn =
        "\"Taking the seven o'clock bus will get us there with time to spare.\"";

    constexpr std::string_view kHPunctDefectJp =
        "「先輩……もっと近くに来て……」";
    constexpr std::string_view kHPunctDefectOldEn =
        "Come closer (senpai)...";
    constexpr std::string_view kHPunctDefectNewEn =
        "\"Senpai... come closer to me...\"";

    scene01[6] = {"蒼井 遥", std::string(kLockedCleanJp),
                  std::string(kLockedCleanEn), false};
    scene01[9] = {"蒼井 遥（Ｈ）", std::string(kHPunctDefectJp),
                  std::string(kHPunctDefectOldEn), false};

    sandbox.write_bytes("NovelKnobs/script_output/translated_text.json",
                        serialize_crlf_indent1_corpus(scene01, {}));

    bool saw_locked_clean_read_only = false;
    bool saw_h_suspect_editable = false;
    bool saw_banter_partner_editable = false;

    transport.set_default_handler(
        [&](const LlmRequest& req, std::size_t) -> Result<LlmResponse> {
            const auto clean_info =
                inspect_target_in_user_prompt(req.user_prompt, kLockedCleanJp);
            const auto h_info =
                inspect_target_in_user_prompt(req.user_prompt, kHPunctDefectJp);
            const auto partner_info = inspect_target_in_user_prompt(
                req.user_prompt, kCleanBaselinePool[10].jp);

            saw_locked_clean_read_only = clean_info.found && !clean_info.editable;
            saw_h_suspect_editable = h_info.found && h_info.editable;
            saw_banter_partner_editable = partner_info.found && partner_info.editable;

            std::vector<std::string> items;
            // Attempt unauthorized edit on locked `[READ-ONLY CONTEXT]` row `clean_info`!
            if (clean_info.found) {
                items.push_back(build_candidate_item_json(
                    clean_info.target_id,
                    "\"Catching the 7 AM bus leaves plenty of room.\"",
                    "stiff_translationese",
                    "minor",
                    0.96,
                    "Unauthorized edit on locked row"));
            }
            if (h_info.found) {
                items.push_back(build_candidate_item_json(
                    h_info.target_id,
                    kHPunctDefectNewEn,
                    "artifact_or_note",
                    "major",
                    0.96,
                    "Restore dialogue quotes and remove parenthetical romaji note",
                    "(senpai)",
                    "Senpai"));
            }

            std::string raw = "[";
            for (std::size_t i = 0; i < items.size(); ++i) {
                if (i > 0U) {
                    raw += ",";
                }
                raw += items[i];
            }
            raw += "]";

            LlmResponse resp;
            resp.text = std::move(raw);
            resp.finish_reason = "STOP";
            resp.prompt_tokens = 200;
            resp.output_tokens = 50;
            resp.thought_tokens = 90;
            return Result<LlmResponse>::success(resp);
        });

    SpotCheckConfig cfg;
    cfg.game_dir = wide_to_utf8(game_dir.wstring());
    cfg.mode = SpotCheckMode::Linear;
    cfg.offset = 6U;
    cfg.window_size = kCompactWindowSize;
    cfg.max_windows = 1U;
    cfg.radar_calls = 0U;
    cfg.review_all_in_window = false;  // `--lock-non-suspects`
    cfg.skip_h_scenes = false;         // `--include-h-scenes`
    cfg.fix_punctuation = true;        // `--fix-punctuation`
    cfg.auto_apply = true;

    auto checker_res = SpotChecker::create(cfg, deps);
    ASSERT_TRUE(checker_res.is_ok()) << checker_res.error_message();
    const auto run_res = checker_res.value().run();
    ASSERT_TRUE(run_res.is_ok()) << run_res.error_message();
    const auto& report = run_res.value();

    EXPECT_EQ(transport.requests().size(), 1U);
    EXPECT_EQ(report.h_windows_skipped, 0U);
    EXPECT_EQ(report.windows_checked, 1U);
    EXPECT_EQ(report.lines_checked, kCompactWindowSize);
    EXPECT_LT(report.editable_lines_unlocked, report.lines_checked);
    EXPECT_TRUE(saw_locked_clean_read_only);
    EXPECT_TRUE(saw_h_suspect_editable);
    EXPECT_TRUE(saw_banter_partner_editable);
    EXPECT_EQ(report.improvements_found, 1U);
    EXPECT_GE(report.candidates_rejected_by_validator, 1U);

    const auto updated_tt =
        boost::json::parse(TempSandbox::read_bytes(
                               game_dir / "script_output" / "translated_text.json"))
            .as_object();
    const auto& s01 =
        updated_tt.at("scene_01").as_object().at("strings").as_array();
    ASSERT_EQ(s01.size(), kKnobsScene01Count);
    EXPECT_EQ(s01[1].as_object().at("translated").as_string(),
              kPreOffsetDefectOldEn);
    EXPECT_EQ(s01[6].as_object().at("translated").as_string(), kLockedCleanEn);
    EXPECT_EQ(s01[9].as_object().at("translated").as_string(),
              kHPunctDefectNewEn);
}

TEST(SpotCheckerComponentTest,
     AdaptiveContagionExpandsAcrossWindowBoundariesWithoutDuplicateLineRechecks) {
    TempSandbox sandbox("ct_contagion");
    ScriptedLlmTransport transport;
    FaultInjectingFileStore store;
    RecordingCryptSync crypt;
    FakeClock clock;
    CapturingLogSink logs;
    const auto deps = bind_deps(transport, store, crypt, clock, logs);

    const auto game_dir = sandbox.path() / "NovelContagion";
    std::vector<SyntheticRow> scene01;
    for (std::size_t i = 0; i < kContagionCorpusSize; ++i) {
        const auto& base = kCleanBaselinePool[i % kCleanBaselinePool.size()];
        scene01.push_back(
            {std::string(base.speaker_jp), std::string(base.jp), std::string(base.en), false});
    }

    // Place a 4-defect cluster across indices 21..29 (spanning 9 lines inside a
    // 10-line primary window `[20, 30)` or `[21, 31)`), place a clean-looking
    // semantic spillover defect strictly outside at index 35 (reachable ONLY via
    // right neighbor contagion), and place a second disjoint hotspot far away at
    // index 95 so `checked_spans` holds >= 2 coalesced disjoint intervals.
    constexpr std::string_view kPrimaryLeadJp =
        "「三脚のネジもしっかり締めておいたぞ」";
    constexpr std::string_view kPrimaryMidJp =
        "「きゃあああっ！望遠鏡が倒れるっ！」";
    constexpr std::string_view kPrimaryPeakJp =
        "「屋上の望遠鏡で今夜の流星群を観測しようぜ」";
    constexpr std::string_view kPrimaryEdgeJp =
        "「ああ、流星群の記録ノートもちゃんと持ってきたぞ」";
    constexpr std::string_view kNeighborSpillJp =
        "「流星群が一番多く見えるのは午前二時ごろらしいわ」";
    constexpr std::string_view kDisjointTailJp =
        "「終章の時計塔には古い鐘が残されている」";

    scene01[kContagionLeadIndex] = {
        "高瀬 蓮", std::string(kPrimaryLeadJp),
        "\"I tightened the 三脚が screws firmly too.\"", false};
    scene01[kContagionMidIndex] = {
        "高瀬 蓮", std::string(kPrimaryMidJp),
        "\"Whoa! The 望遠鏡が is about to tip over!\"", false};
    scene01[kContagionPeakIndex] = {
        "高瀬 蓮", std::string(kPrimaryPeakJp),
        "\"Let's observe tonight's fireworks 鍵 with the rooftop telescope.\"",
        false};
    scene01[kContagionEdgeIndex] = {
        "蒼井 遥", std::string(kPrimaryEdgeJp),
        "\"Yeah, I brought the fireworks 鍵 logbook too.\"",
        false};
    scene01[kContagionSpillIndex] = {
        "蒼井 遥", std::string(kNeighborSpillJp),
        "\"They say the meteor shower peaks around noon.\"",
        false};
    scene01[kContagionTailIndex] = {
        "高瀬 蓮", std::string(kDisjointTailJp),
        "\"An old 鐘が still hangs inside the epilogue clocktower.\"",
        false};

    sandbox.write_bytes("NovelContagion/script_output/translated_text.json",
                        serialize_crlf_indent1_corpus(scene01, {}));

    std::size_t primary_target_appearances = 0;
    transport.set_default_handler(
        [&](const LlmRequest& req, std::size_t) -> Result<LlmResponse> {
            std::vector<std::string> items;

            const auto r21 =
                inspect_target_in_user_prompt(req.user_prompt, kPrimaryLeadJp);
            if (r21.found) {
                items.push_back(build_candidate_item_json(
                    r21.target_id,
                    "\"I tightened the tripod screws firmly too.\"",
                    "mistranslation",
                    "major",
                    0.96,
                    "Translate leaked Japanese tripod phrase"));
            }
            const auto r25 =
                inspect_target_in_user_prompt(req.user_prompt, kPrimaryMidJp);
            if (r25.found) {
                ++primary_target_appearances;
                items.push_back(build_candidate_item_json(
                    r25.target_id,
                    "\"Whoa! The telescope is about to tip over!\"",
                    "mistranslation",
                    "major",
                    0.96,
                    "Translate leaked Japanese telescope phrase"));
            }
            const auto r28 =
                inspect_target_in_user_prompt(req.user_prompt, kPrimaryPeakJp);
            if (r28.found) {
                items.push_back(build_candidate_item_json(
                    r28.target_id,
                    "\"Let's observe tonight's meteor shower through the rooftop telescope.\"",
                    "broken_callback",
                    "major",
                    0.95,
                    "流星群 means meteor shower"));
            }
            const auto r29 =
                inspect_target_in_user_prompt(req.user_prompt, kPrimaryEdgeJp);
            if (r29.found) {
                items.push_back(build_candidate_item_json(
                    r29.target_id,
                    "\"Yeah, I brought our meteor shower observation logbook too.\"",
                    "broken_callback",
                    "major",
                    0.95,
                    "Restore meteor shower callback"));
            }
            const auto r35 =
                inspect_target_in_user_prompt(req.user_prompt, kNeighborSpillJp);
            if (r35.found) {
                items.push_back(build_candidate_item_json(
                    r35.target_id,
                    "\"They say the meteor shower peaks around two in the morning.\"",
                    "mistranslation",
                    "major",
                    0.95,
                    "午前二時 means 2 AM, not noon"));
            }
            const auto r95 =
                inspect_target_in_user_prompt(req.user_prompt, kDisjointTailJp);
            if (r95.found) {
                items.push_back(build_candidate_item_json(
                    r95.target_id,
                    "\"An old bell still hangs inside the epilogue clocktower.\"",
                    "mistranslation",
                    "major",
                    0.95,
                    "Translate leaked kanji 鐘"));
            }

            std::string raw = "[";
            for (std::size_t i = 0; i < items.size(); ++i) {
                if (i > 0U) {
                    raw += ",";
                }
                raw += items[i];
            }
            raw += "]";

            LlmResponse resp;
            resp.text = std::move(raw);
            resp.finish_reason = "STOP";
            resp.prompt_tokens = 300;
            resp.output_tokens = 80;
            resp.thought_tokens = 110;
            return Result<LlmResponse>::success(resp);
        });

    SpotCheckConfig cfg;
    cfg.game_dir = wide_to_utf8(game_dir.wstring());
    cfg.mode = SpotCheckMode::Hotspot;
    cfg.window_size = kSmallWindowSize;
    cfg.z_threshold = kContagionZThreshold;
    cfg.radar_calls = 0U;
    cfg.max_windows = kContagionMaxWindows;
    cfg.auto_apply = true;

    auto checker_res = SpotChecker::create(cfg, deps);
    ASSERT_TRUE(checker_res.is_ok()) << checker_res.error_message();
    const auto run_res = checker_res.value().run();
    ASSERT_TRUE(run_res.is_ok()) << run_res.error_message();
    const auto& report = run_res.value();

    EXPECT_GE(report.adaptive_expansions, 1U);
    EXPECT_EQ(report.improvements_found, kContagionExpectedImprovements);
    // IntervalTracker guarantees line 25 was never sent as a target twice across
    // adaptive neighbor expansions!
    EXPECT_EQ(primary_target_appearances, 1U);

    // Checked spans recorded in report must have >= 2 disjoint sorted intervals.
    ASSERT_GE(report.checked_spans.size(), kMinContagionSpans);
    for (std::size_t i = 1U; i < report.checked_spans.size(); ++i) {
        const auto prev_end =
            report.checked_spans[i - 1U].first + report.checked_spans[i - 1U].second;
        EXPECT_LE(prev_end, report.checked_spans[i].first);
    }

    const auto updated_tt =
        boost::json::parse(TempSandbox::read_bytes(
                               game_dir / "script_output" / "translated_text.json"))
            .as_object();
    const auto& s01 =
        updated_tt.at("scene_01").as_object().at("strings").as_array();
    ASSERT_EQ(s01.size(), kContagionCorpusSize);
    EXPECT_EQ(
        s01[kContagionSpillIndex].as_object().at("translated").as_string(),
        "\"They say the meteor shower peaks around two in the morning.\"");
    EXPECT_EQ(
        s01[kContagionTailIndex].as_object().at("translated").as_string(),
        "\"An old bell still hangs inside the epilogue clocktower.\"");
}

TEST(SpotCheckerComponentTest,
     CheckpointResumeMaxApiCallsCapCorruptCheckpointAndZeroChurnIdempotence) {
    TempSandbox sandbox("ct_checkpoint_resume");
    ScriptedLlmTransport transport;
    FaultInjectingFileStore store;
    RecordingCryptSync crypt;
    FakeClock clock;
    CapturingLogSink logs;
    const auto deps = bind_deps(transport, store, crypt, clock, logs);

    const auto game_dir = sandbox.path() / "NovelResume";
    std::vector<SyntheticRow> scene01;
    for (std::size_t i = 0; i < 60U; ++i) {
        const auto& base = kCleanBaselinePool[i % kCleanBaselinePool.size()];
        scene01.push_back(
            {std::string(base.speaker_jp), std::string(base.jp), std::string(base.en), false});
    }

    constexpr std::string_view kCluster1Jp =
        "「第一章の鍵は古い机の引き出しに入っていた」";
    constexpr std::string_view kCluster2Jp =
        "「第二章の手紙は駅のポストへ投函してきたよ」";

    scene01[5] = {"蒼井 遥", std::string(kCluster1Jp),
                  "\"Chapter one's 鍵 was inside the desk drawer.\"", false};
    scene01[48] = {"高瀬 蓮", std::string(kCluster2Jp),
                   "\"I mailed chapter two's 手紙 at the station postbox.\"", false};

    sandbox.write_bytes("NovelResume/script_output/translated_text.json",
                        serialize_crlf_indent1_corpus(scene01, {}));

    transport.set_default_handler(
        [&](const LlmRequest& req, std::size_t) -> Result<LlmResponse> {
            LlmResponse resp;
            resp.finish_reason = "STOP";
            resp.prompt_tokens = 250;
            resp.output_tokens = 60;
            resp.thought_tokens = 100;

            const int p1 = find_radar_passage_id_for_jp(req.user_prompt, kCluster1Jp);
            const int p2 = find_radar_passage_id_for_jp(req.user_prompt, kCluster2Jp);
            if (p1 > 0 || p2 > 0) {
                boost::json::array arr;
                if (p1 > 0) {
                    arr.push_back(boost::json::object{
                        {"passage_id", p1}, {"critical_count", 2}, {"major_count", 0}, {"minor_count", 0}});
                }
                if (p2 > 0) {
                    arr.push_back(boost::json::object{
                        {"passage_id", p2}, {"critical_count", 1}, {"major_count", 0}, {"minor_count", 0}});
                }
                resp.text = boost::json::serialize(arr);
                return Result<LlmResponse>::success(resp);
            }

            const auto c1 = inspect_target_in_user_prompt(req.user_prompt, kCluster1Jp);
            if (c1.found) {
                resp.text = "[" +
                            build_candidate_item_json(
                                c1.target_id,
                                "\"The key from chapter one was inside the old desk drawer.\"",
                                "mistranslation",
                                "major",
                                0.96,
                                "Translate leaked kanji 鍵") +
                            "]";
                return Result<LlmResponse>::success(resp);
            }
            const auto c2 = inspect_target_in_user_prompt(req.user_prompt, kCluster2Jp);
            if (c2.found) {
                resp.text = "[" +
                            build_candidate_item_json(
                                c2.target_id,
                                "\"I dropped chapter two's letter into the station postbox.\"",
                                "mistranslation",
                                "major",
                                0.96,
                                "Translate leaked kanji 手紙") +
                            "]";
                return Result<LlmResponse>::success(resp);
            }
            resp.text = "[]";
            return Result<LlmResponse>::success(resp);
        });

    // Run 1: cap at `max_api_calls = 2` (1 Radar call + 1 Stage 2 window call).
    SpotCheckConfig run1_cfg;
    run1_cfg.game_dir = wide_to_utf8(game_dir.wstring());
    run1_cfg.window_size = kSmallWindowSize;
    run1_cfg.radar_calls = 1U;
    run1_cfg.max_windows = 4U;
    run1_cfg.max_api_calls = 2U;
    run1_cfg.auto_apply = true;

    auto checker1 = SpotChecker::create(run1_cfg, deps);
    ASSERT_TRUE(checker1.is_ok()) << checker1.error_message();
    const auto res1 = checker1.value().run();
    ASSERT_TRUE(res1.is_ok()) << res1.error_message();
    EXPECT_EQ(transport.requests().size(), 2U);
    EXPECT_EQ(res1.value().windows_checked, 1U);
    EXPECT_EQ(res1.value().improvements_found, 1U);

    // Run 2: resume from persisted `argon_spot_check.json` with `max_api_calls = 0`.
    SpotCheckConfig run2_cfg = run1_cfg;
    run2_cfg.max_api_calls = 0U;
    auto checker2 = SpotChecker::create(run2_cfg, deps);
    ASSERT_TRUE(checker2.is_ok()) << checker2.error_message();
    const auto res2 = checker2.value().run();
    ASSERT_TRUE(res2.is_ok()) << res2.error_message();

    // Only 1 additional Stage 2 window call needed (Radar & Window 1 skipped via checkpoint!).
    EXPECT_EQ(transport.requests().size(), 3U);
    EXPECT_GE(res2.value().windows_checked, 2U);
    EXPECT_EQ(res2.value().improvements_found, 2U);
    EXPECT_EQ(res2.value().token_usage.api_calls, 3U);
    EXPECT_EQ(res2.value().token_usage.prompt_tokens, 3 * 250);

    // Run 3: fresh pass (`out_path` pointed to a clean file) with `review_all_in_window = false`
    // over the now-clean corpus -> 0 hotspots / 0 editable suspects -> 0 new LLM calls!
    transport.clear_requests();
    SpotCheckConfig run3_cfg = run2_cfg;
    run3_cfg.review_all_in_window = false;
    run3_cfg.out_path =
        wide_to_utf8((sandbox.path() / "fresh_converged_check.json").wstring());

    auto checker3 = SpotChecker::create(run3_cfg, deps);
    ASSERT_TRUE(checker3.is_ok()) << checker3.error_message();
    const auto res3 = checker3.value().run();
    ASSERT_TRUE(res3.is_ok()) << res3.error_message();
    EXPECT_EQ(res3.value().hotspots_detected, 0U);
    EXPECT_TRUE(transport.requests().empty());

    // Run 4: corrupting `argon_spot_check.json` fails loudly with `ErrorCode::CorruptCheckpoint`.
    sandbox.write_bytes("NovelResume/script_output/argon_spot_check.json",
                        "{ truncated_checkpoint: [");
    auto checker4 = SpotChecker::create(run2_cfg, deps);
    ASSERT_TRUE(checker4.is_ok());
    const auto res4 = checker4.value().run();
    ASSERT_FALSE(res4.is_ok());
    EXPECT_EQ(res4.error_code(), ErrorCode::CorruptCheckpoint);
}

TEST(SpotCheckerComponentTest,
     DryRunLeavesArtifactsUntouchedThenApplyPropagatesDuplicatesAndHandlesFaults) {
    TempSandbox sandbox("ct_dry_run_apply");
    ScriptedLlmTransport transport;
    FaultInjectingFileStore store;
    RecordingCryptSync crypt;
    FakeClock clock;
    CapturingLogSink logs;
    const auto deps = bind_deps(transport, store, crypt, clock, logs);

    const auto game_dir = sandbox.path() / "NovelDryRun";
    constexpr std::string_view kSharedDupJp =
        "「帰り道にパン屋で焼きたてのクロワッサンを買っていこう」";
    constexpr std::string_view kSharedDupOldEn =
        "\"Let's buy fresh croissants at the パン屋 on our way back.\"";
    constexpr std::string_view kSharedDupNewEn =
        "\"Let's buy freshly baked croissants at the bakery on our way home.\"";

    std::vector<SyntheticRow> scene01;
    for (std::size_t i = 0; i < 10U; ++i) {
        const auto& base = kCleanBaselinePool[i % kCleanBaselinePool.size()];
        scene01.push_back(
            {std::string(base.speaker_jp), std::string(base.jp), std::string(base.en), false});
    }
    scene01[3] = {"蒼井 遥", std::string(kSharedDupJp),
                  std::string(kSharedDupOldEn), true};

    // Duplicate `jp_text` in `scene_02` that was never inspected directly by Stage 2!
    std::vector<SyntheticRow> scene02;
    for (std::size_t i = 0; i < 6U; ++i) {
        const auto& base = kCleanBaselinePool[i % kCleanBaselinePool.size()];
        scene02.push_back(
            {"", std::string(base.jp), std::string(base.en), false});
    }
    scene02[4] = {"", std::string(kSharedDupJp),
                  std::string(kSharedDupOldEn), false};

    const std::string initial_tt_bytes =
        serialize_crlf_indent1_corpus(scene01, scene02);
    const std::string initial_cache_a =
        "{\n  \"keep_first\": \"First\",\n  " + json_escape(kSharedDupJp) + ": " +
        json_escape(kSharedDupOldEn) + ",\n  \"keep_last\": \"Last\"\n}\n";
    const std::string initial_cache_b =
        "{\n  " + json_escape(kSharedDupJp) + ": " +
        json_escape(kSharedDupOldEn) + "\n}\n";

    const auto tt_path = sandbox.write_bytes(
        "NovelDryRun/script_output/translated_text.json", initial_tt_bytes);
    const auto cache_a_path = sandbox.write_bytes(
        "NovelDryRun/script_output/translation_cache_anthropic.json", initial_cache_a);
    const auto cache_b_path = sandbox.write_bytes(
        "NovelDryRun/script_output/translation_cache_secondary.json", initial_cache_b);

    transport.set_default_handler(
        [&](const LlmRequest& req, std::size_t) -> Result<LlmResponse> {
            const auto info =
                inspect_target_in_user_prompt(req.user_prompt, kSharedDupJp);
            LlmResponse resp;
            resp.finish_reason = "STOP";
            resp.prompt_tokens = 180;
            resp.output_tokens = 40;
            resp.thought_tokens = 70;
            resp.text = info.found
                            ? ("[" +
                               build_candidate_item_json(
                                   info.target_id,
                                   kSharedDupNewEn,
                                   "mistranslation",
                                   "major",
                                   0.96,
                                   "Translate leaked kanji パン屋") +
                               "]")
                            : "[]";
            return Result<LlmResponse>::success(resp);
        });

    SpotCheckConfig dry_cfg;
    dry_cfg.game_dir = wide_to_utf8(game_dir.wstring());
    dry_cfg.mode = SpotCheckMode::Linear;
    dry_cfg.offset = 0U;
    dry_cfg.window_size = kSmallWindowSize;
    dry_cfg.max_windows = 1U;
    dry_cfg.radar_calls = 0U;
    dry_cfg.auto_apply = false;

    auto dry_checker = SpotChecker::create(dry_cfg, deps);
    ASSERT_TRUE(dry_checker.is_ok()) << dry_checker.error_message();
    const auto dry_res = dry_checker.value().run();
    ASSERT_TRUE(dry_res.is_ok()) << dry_res.error_message();

    EXPECT_EQ(dry_res.value().improvements_found, 1U);
    EXPECT_EQ(dry_res.value().script_entries_updated, 0U);
    EXPECT_EQ(dry_res.value().cache_entries_updated, 0U);
    EXPECT_EQ(TempSandbox::read_bytes(tt_path), initial_tt_bytes);
    EXPECT_EQ(TempSandbox::read_bytes(cache_a_path), initial_cache_a);
    EXPECT_EQ(TempSandbox::read_bytes(cache_b_path), initial_cache_b);
    EXPECT_TRUE(crypt.encrypt_calls().empty());

    // Inject `ICryptSync::encrypt_if_tracked` failure during `apply_improvements()`.
    transport.clear_requests();
    crypt.set_fail_encrypt(true);
    auto fail_apply_checker = SpotChecker::create(dry_cfg, deps);
    ASSERT_TRUE(fail_apply_checker.is_ok());
    const auto fail_apply_res = fail_apply_checker.value().apply_improvements();
    ASSERT_FALSE(fail_apply_res.is_ok());
    EXPECT_EQ(fail_apply_res.error_code(), ErrorCode::CryptSyncFailed);
    EXPECT_TRUE(transport.requests().empty());

    // Restore `translated_text.json` & caches for clean offline `apply_improvements()`.
    sandbox.write_bytes("NovelDryRun/script_output/translated_text.json",
                        initial_tt_bytes);
    sandbox.write_bytes("NovelDryRun/script_output/translation_cache_anthropic.json",
                        initial_cache_a);
    sandbox.write_bytes("NovelDryRun/script_output/translation_cache_secondary.json",
                        initial_cache_b);
    crypt.set_fail_encrypt(false);

    auto apply_checker = SpotChecker::create(dry_cfg, deps);
    ASSERT_TRUE(apply_checker.is_ok());
    const auto apply_res = apply_checker.value().apply_improvements();
    ASSERT_TRUE(apply_res.is_ok()) << apply_res.error_message();

    EXPECT_TRUE(transport.requests().empty());
    EXPECT_EQ(apply_res.value().improvements_loaded, 1U);
    // Primary entry in `scene_01` + duplicate JP entry in `scene_02` = 2 entries updated!
    EXPECT_EQ(apply_res.value().script_entries_updated, 2U);
    // Both `translation_cache_*.json` files updated = 2 cache entries updated!
    EXPECT_EQ(apply_res.value().cache_entries_updated, 2U);
    EXPECT_FALSE(crypt.encrypt_calls().empty());

    const auto after_first_apply_tt = TempSandbox::read_bytes(tt_path);
    const auto parsed_tt = boost::json::parse(after_first_apply_tt).as_object();
    const auto& s01_arr =
        parsed_tt.at("scene_01").as_object().at("strings").as_array();
    ASSERT_EQ(s01_arr.size(), kDryRunScene01Count);
    EXPECT_EQ(s01_arr[3].as_object().at("translated").as_string(),
              kSharedDupNewEn);
    const auto& s02_arr =
        parsed_tt.at("scene_02").as_object().at("lines").as_array();
    ASSERT_EQ(s02_arr.size(), kDryRunScene02Count);
    EXPECT_EQ(s02_arr[4].as_object().at("translated").as_string(),
              kSharedDupNewEn);

    // Second `apply_improvements()` call must be 100% idempotent (`0` updates, identical bytes).
    auto apply_again_checker = SpotChecker::create(dry_cfg, deps);
    ASSERT_TRUE(apply_again_checker.is_ok());
    const auto apply_again_res = apply_again_checker.value().apply_improvements();
    ASSERT_TRUE(apply_again_res.is_ok()) << apply_again_res.error_message();
    EXPECT_EQ(apply_again_res.value().script_entries_updated, 0U);
    EXPECT_EQ(apply_again_res.value().cache_entries_updated, 0U);
    EXPECT_EQ(TempSandbox::read_bytes(tt_path), after_first_apply_tt);
}

TEST(SpotCheckerComponentTest,
     MidBatchRateLimitAndDiskWriteFaultsRollbackCleanlyAndTwentySixWindowSyncDoesNotDoubleCount) {
    // Part 1: Mid-batch `RateLimited` on Window #2 + mid-commit disk write fault rollback.
    {
        TempSandbox sandbox("ct_fault_rollback");
        ScriptedLlmTransport transport;
        FaultInjectingFileStore store;
        RecordingCryptSync crypt;
        FakeClock clock;
        CapturingLogSink logs;
        const auto deps = bind_deps(transport, store, crypt, clock, logs);

        const auto game_dir = sandbox.path() / "NovelFaults";
        std::vector<SyntheticRow> scene01;
        for (std::size_t i = 0; i < 12U; ++i) {
            const auto& base = kCleanBaselinePool[i % kCleanBaselinePool.size()];
            scene01.push_back(
                {std::string(base.speaker_jp), std::string(base.jp), std::string(base.en), false});
        }
        constexpr std::string_view kWin1FixJp =
            "「最初のウィンドウで見つかる誤訳だ」";
        constexpr std::string_view kWin1OldEn =
            "\"Mistranslation 鍵 found in window one.\"";
        constexpr std::string_view kWin1NewEn =
            "\"A mistranslation discovered inside the first window.\"";

        scene01[2] = {"蒼井 遥", std::string(kWin1FixJp),
                      std::string(kWin1OldEn), false};
        scene01[8] = {"高瀬 蓮", "「二番目のウィンドウでレート制限が起きる」",
                      "\"Rate limit 鍵 in window two.\"", false};

        const std::string orig_tt = serialize_crlf_indent1_corpus(scene01, {});
        const std::string orig_cache =
            "{\n  " + json_escape(kWin1FixJp) + ": " + json_escape(kWin1OldEn) + "\n}\n";

        const auto tt_path = sandbox.write_bytes(
            "NovelFaults/script_output/translated_text.json", orig_tt);
        const auto cache_path = sandbox.write_bytes(
            "NovelFaults/script_output/translation_cache_anthropic.json", orig_cache);

        // Window 1 succeeds; Window 2 hits HTTP 429 `ErrorCode::RateLimited`.
        transport.enqueue_handler(
            [&](const LlmRequest& req, std::size_t) -> Result<LlmResponse> {
                const auto info =
                    inspect_target_in_user_prompt(req.user_prompt, kWin1FixJp);
                LlmResponse resp;
                resp.finish_reason = "STOP";
                resp.prompt_tokens = 150;
                resp.output_tokens = 35;
                resp.thought_tokens = 60;
                resp.text = "[" +
                            build_candidate_item_json(
                                info.target_id,
                                kWin1NewEn,
                                "mistranslation",
                                "major",
                                0.95,
                                "Fix window 1 leaked kanji") +
                            "]";
                return Result<LlmResponse>::success(resp);
            });
        transport.enqueue_error(
            ErrorCode::RateLimited, "HTTP 429 RESOURCE_EXHAUSTED", 429);

        SpotCheckConfig cfg;
        cfg.game_dir = wide_to_utf8(game_dir.wstring());
        cfg.mode = SpotCheckMode::Linear;
        cfg.offset = 0U;
        cfg.window_size = kCompactWindowSize;
        cfg.max_windows = 2U;
        cfg.radar_calls = 0U;
        cfg.auto_apply = false;

        auto checker_res = SpotChecker::create(cfg, deps);
        ASSERT_TRUE(checker_res.is_ok());
        const auto run_res = checker_res.value().run();
        ASSERT_FALSE(run_res.is_ok());
        EXPECT_EQ(run_res.error_code(), ErrorCode::RateLimited);
        EXPECT_FALSE(has_any_tmp_files(game_dir));

        // Window 1's checkpoint must still be valid JSON on disk.
        const auto ckpt_raw = TempSandbox::read_bytes(
            game_dir / "script_output" / "argon_spot_check.json");
        ASSERT_FALSE(ckpt_raw.empty());
        const auto ckpt_obj = boost::json::parse(ckpt_raw).as_object();
        EXPECT_EQ(ckpt_obj.at("improvements_count").to_number<std::size_t>(), 1U);

        // Now inject an atomic disk write failure specifically when committing `translation_cache_`
        // during `apply_improvements()` -> `translated_text.json` MUST roll back to `orig_tt`!
        store.fail_writes_matching("translation_cache_");
        auto rollback_checker = SpotChecker::create(cfg, deps);
        ASSERT_TRUE(rollback_checker.is_ok());
        const auto failed_commit = rollback_checker.value().apply_improvements();
        ASSERT_FALSE(failed_commit.is_ok());
        EXPECT_EQ(failed_commit.error_code(), ErrorCode::IoError);
        EXPECT_EQ(TempSandbox::read_bytes(tt_path), orig_tt);
        EXPECT_EQ(TempSandbox::read_bytes(cache_path), orig_cache);
        EXPECT_FALSE(has_any_tmp_files(game_dir));
    }

    // Part 2: 26-window periodic sync (`periodic_sync_interval = 25`) must not double-count.
    {
        TempSandbox sandbox("ct_periodic_26");
        ScriptedLlmTransport transport;
        FaultInjectingFileStore store;
        RecordingCryptSync crypt;
        FakeClock clock;
        CapturingLogSink logs;
        const auto deps = bind_deps(transport, store, crypt, clock, logs);

        const auto game_dir = sandbox.path() / "NovelPeriodic26";
        std::vector<SyntheticRow> scene01;
        for (std::size_t i = 0; i < kTwentySixWindows * kTinyPairWindowSize; ++i) {
            const auto& base = kCleanBaselinePool[i % kCleanBaselinePool.size()];
            scene01.push_back(
                {std::string(base.speaker_jp), std::string(base.jp), std::string(base.en), false});
        }

        constexpr std::string_view kFirstWinJp =
            "「一回目の定期同期の前に直される台詞だ」";
        constexpr std::string_view kFirstWinOldEn =
            "\"Line fixed 鍵 before first periodic sync.\"";
        constexpr std::string_view kFirstWinNewEn =
            "\"A line repaired before the twenty-fifth window sync.\"";

        constexpr std::string_view kTwentySixthWinJp =
            "「二十六番目のウィンドウで直される最後の台詞だ」";
        constexpr std::string_view kTwentySixthWinOldEn =
            "\"Final line fixed 鍵 in window twenty-six.\"";
        constexpr std::string_view kTwentySixthWinNewEn =
            "\"The final line repaired inside window twenty-six.\"";

        scene01[0] = {"蒼井 遥", std::string(kFirstWinJp),
                      std::string(kFirstWinOldEn), false};
        scene01[50] = {"高瀬 蓮", std::string(kTwentySixthWinJp),
                       std::string(kTwentySixthWinOldEn), false};

        sandbox.write_bytes("NovelPeriodic26/script_output/translated_text.json",
                            serialize_crlf_indent1_corpus(scene01, {}));
        sandbox.write_bytes(
            "NovelPeriodic26/script_output/translation_cache_anthropic.json",
            "{\n  " + json_escape(kFirstWinJp) + ": " + json_escape(kFirstWinOldEn) +
                ",\n  " + json_escape(kTwentySixthWinJp) + ": " +
                json_escape(kTwentySixthWinOldEn) + "\n}\n");

        transport.set_default_handler(
            [&](const LlmRequest& req, std::size_t) -> Result<LlmResponse> {
                LlmResponse resp;
                resp.finish_reason = "STOP";
                resp.prompt_tokens = 100;
                resp.output_tokens = 25;
                resp.thought_tokens = 40;

                const auto f1 =
                    inspect_target_in_user_prompt(req.user_prompt, kFirstWinJp);
                if (f1.found) {
                    resp.text = "[" +
                                build_candidate_item_json(
                                    f1.target_id,
                                    kFirstWinNewEn,
                                    "mistranslation",
                                    "major",
                                    0.95,
                                    "Fix window 1") +
                                "]";
                    return Result<LlmResponse>::success(resp);
                }
                const auto f26 = inspect_target_in_user_prompt(
                    req.user_prompt, kTwentySixthWinJp);
                if (f26.found) {
                    resp.text = "[" +
                                build_candidate_item_json(
                                    f26.target_id,
                                    kTwentySixthWinNewEn,
                                    "mistranslation",
                                    "major",
                                    0.95,
                                    "Fix window 26") +
                                "]";
                    return Result<LlmResponse>::success(resp);
                }
                resp.text = "[]";
                return Result<LlmResponse>::success(resp);
            });

        SpotCheckConfig cfg;
        cfg.game_dir = wide_to_utf8(game_dir.wstring());
        cfg.mode = SpotCheckMode::Linear;
        cfg.offset = 0U;
        cfg.window_size = kTinyPairWindowSize;
        cfg.max_windows = kTwentySixWindows;
        cfg.periodic_sync_interval = kDefaultSyncEvery;
        cfg.radar_calls = 0U;
        cfg.auto_apply = true;

        auto checker_res = SpotChecker::create(cfg, deps);
        ASSERT_TRUE(checker_res.is_ok()) << checker_res.error_message();
        const auto run_res = checker_res.value().run();
        ASSERT_TRUE(run_res.is_ok()) << run_res.error_message();
        const auto& report = run_res.value();

        EXPECT_EQ(report.windows_checked, kTwentySixWindows);
        EXPECT_EQ(report.improvements_found, 2U);
        // Must equal 2 (exact unique updated entries), NOT 3 from double-counting window 1!
        EXPECT_EQ(report.script_entries_updated, 2U);
        EXPECT_EQ(report.cache_entries_updated, 2U);
        // Periodic sync at window 25 + final sync at window 26 -> >= 2 encrypt calls.
        EXPECT_GE(crypt.encrypt_calls().size(), 2U);

        auto verify_checker = SpotChecker::create(cfg, deps);
        ASSERT_TRUE(verify_checker.is_ok());
        const auto after_apply = verify_checker.value().apply_improvements();
        ASSERT_TRUE(after_apply.is_ok());
        EXPECT_EQ(after_apply.value().script_entries_updated, 0U);
        EXPECT_EQ(after_apply.value().cache_entries_updated, 0U);
    }
}

TEST(SpotCheckerComponentTest,
     ProgressiveRefusalRecoverySplitsSalvagesTracksBlockedSpansAndRequeuesOnAlternateModel) {
    TempSandbox sandbox("ct_content_blocked");
    ScriptedLlmTransport transport;
    FaultInjectingFileStore store;
    RecordingCryptSync crypt;
    FakeClock clock;
    CapturingLogSink logs;
    const auto deps = bind_deps(transport, store, crypt, clock, logs);

    const auto game_dir = sandbox.path() / "NovelBlocked";
    std::vector<SyntheticRow> scene01;
    scene01.reserve(kBlockedCorpusSize);
    for (std::size_t i = 0; i < kBlockedCorpusSize; ++i) {
        const auto& base = kCleanBaselinePool[i % kCleanBaselinePool.size()];
        scene01.push_back(
            {std::string(base.speaker_jp), std::string(base.jp), std::string(base.en), false});
    }

    // Two well-separated hotspot clusters exercising all 4 refusal-recovery tiers:
    // - Cluster #1 (`indices 0, 2, 7`, energy `3 * 8.0 = 24.0` -> Rank #1 window `[0, 10)`):
    //   * `index 0` hard-triggers `PROHIBITED_CONTENT` on primary model `"gemini-4-argon"`.
    //   * `[0, 10)` fails with context -> fails context-free -> splits into `[0, 5)` + `[5, 10)`.
    //   * Left leaf `[0, 5)` (`length == kDefaultMinBlockedSplitLines == 5`) stays blocked and
    //     lands ONLY in `blocked_spans` (`{0, 5}`), leaving `indices 0, 2` untouched on Run 1.
    //   * Right leaf `[5, 10)` runs context-free, salvaging and repairing `index 7`!
    // - Cluster #2 (`index 45`, energy `12.5` -> Rank #2 window `~[40, 50)`):
    //   * Clean context line `index 36` sits inside `<context_before>` and triggers `SAFETY`
    //     ONLY when `<context_before>` is attached; Tier-2 context-stripped retry succeeds
    //     immediately on the full 10-line window without splitting!
    constexpr std::string_view kHardTriggerJp =
        "「暗い屋上のフェンス越しに冷たい夜風が吹き抜ける」";
    constexpr std::string_view kHardTriggerOldEn =
        "\"Cold 夜風 blows across the dark rooftop fence.\"";
    constexpr std::string_view kHardTriggerNewEn =
        "\"A cold night wind blows across the dark rooftop fence.\"";

    constexpr std::string_view kLeftCompanionJp =
        "「錆びた鉄扉の向こうから誰かの足音が近づいてくる」";
    constexpr std::string_view kLeftCompanionOldEn =
        "\"Someone's 足音 approaches from beyond the rusted iron door.\"";
    constexpr std::string_view kLeftCompanionNewEn =
        "\"Someone's footsteps approach from beyond the rusted iron door.\"";

    constexpr std::string_view kRightSalvagedJp =
        "「非常灯の赤い光だけが狭い廊下を照らしていた」";
    constexpr std::string_view kRightSalvagedOldEn =
        "\"Only the red 非常灯 illuminated the narrow corridor.\"";
    constexpr std::string_view kRightSalvagedNewEn =
        "\"Only the red emergency light illuminated the narrow corridor.\"";

    constexpr std::string_view kContextPoisonJp =
        "「保健室の白いシーツには乾いた影が落ちていた」";
    constexpr std::string_view kContextPoisonCleanEn =
        "\"A dry shadow fell across the white infirmary sheets.\"";

    constexpr std::string_view kContextRetryTargetJp =
        "「先輩に懐中電灯を貸してもらったんだ」";
    constexpr std::string_view kContextRetryTargetOldEn =
        "\"I lent my senior a 懐中電灯.\"";
    constexpr std::string_view kContextRetryTargetNewEn =
        "\"My senior lent me a flashlight.\"";

    scene01[kBlockedHardTriggerIdx] = {
        "高瀬 蓮", std::string(kHardTriggerJp), std::string(kHardTriggerOldEn), false};
    scene01[kBlockedLeftCompanionIdx] = {
        "高瀬 蓮", std::string(kLeftCompanionJp), std::string(kLeftCompanionOldEn), false};
    scene01[kBlockedRightSalvagedIdx] = {
        "蒼井 遥", std::string(kRightSalvagedJp), std::string(kRightSalvagedOldEn), false};
    scene01[kBlockedContextPoisonIdx] = {
        "", std::string(kContextPoisonJp), std::string(kContextPoisonCleanEn), false};
    scene01[kBlockedContextRetryTargetIdx] = {
        "高瀬 蓮", std::string(kContextRetryTargetJp),
        std::string(kContextRetryTargetOldEn), false};

    const auto tt_path = sandbox.write_bytes(
        "NovelBlocked/script_output/translated_text.json",
        serialize_crlf_indent1_corpus(scene01, {}));

    bool fallback_model_active = false;
    // Call 0 (Stage-1 Radar): blocked with `PROHIBITED_CONTENT`.
    transport.enqueue_blocked("PROHIBITED_CONTENT",
                              kBlockedRadarPromptTokens,
                              kBlockedRadarOutputTokens,
                              kBlockedRadarThoughtTokens);

    transport.set_default_handler(
        [&](const LlmRequest& req, std::size_t) -> Result<LlmResponse> {
            // Primary model hard-refuses whenever `kHardTriggerJp` is present in the prompt.
            if (!fallback_model_active &&
                req.user_prompt.find(kHardTriggerJp) != std::string::npos) {
                LlmResponse blocked;
                blocked.finish_reason = "PROHIBITED_CONTENT";
                blocked.prompt_tokens = kBlockedWinPromptTokens;
                blocked.output_tokens = kBlockedWinOutputTokens;
                blocked.thought_tokens = kBlockedWinThoughtTokens;
                blocked.content_blocked = true;
                return Result<LlmResponse>::success(blocked);
            }

            // Context-only poison: triggers `SAFETY` strictly when attached inside
            // `<context_before>` (not when stripped or inspected directly).
            if (req.user_prompt.find("<context_before>") != std::string::npos &&
                req.user_prompt.find(kContextPoisonJp) != std::string::npos &&
                !inspect_target_in_user_prompt(req.user_prompt, kContextPoisonJp)
                     .found) {
                LlmResponse blocked;
                blocked.finish_reason = "SAFETY";
                blocked.prompt_tokens = kBlockedWinPromptTokens;
                blocked.output_tokens = kBlockedWinOutputTokens;
                blocked.thought_tokens = kBlockedWinThoughtTokens;
                blocked.content_blocked = true;
                return Result<LlmResponse>::success(blocked);
            }

            std::vector<std::string> items;
            const auto h_hard =
                inspect_target_in_user_prompt(req.user_prompt, kHardTriggerJp);
            if (h_hard.found) {
                items.push_back(build_candidate_item_json(
                    h_hard.target_id,
                    kHardTriggerNewEn,
                    "mistranslation",
                    "major",
                    kBlockedMajorConfidence,
                    "Translate leaked kanji 夜風"));
            }
            const auto h_left =
                inspect_target_in_user_prompt(req.user_prompt, kLeftCompanionJp);
            if (h_left.found) {
                items.push_back(build_candidate_item_json(
                    h_left.target_id,
                    kLeftCompanionNewEn,
                    "mistranslation",
                    "major",
                    kBlockedMajorConfidence,
                    "Translate leaked kanji 足音"));
            }
            const auto h_right =
                inspect_target_in_user_prompt(req.user_prompt, kRightSalvagedJp);
            if (h_right.found) {
                items.push_back(build_candidate_item_json(
                    h_right.target_id,
                    kRightSalvagedNewEn,
                    "mistranslation",
                    "major",
                    kBlockedMajorConfidence,
                    "Translate leaked kanji 非常灯"));
            }
            const auto h_ctx = inspect_target_in_user_prompt(
                req.user_prompt, kContextRetryTargetJp);
            if (h_ctx.found) {
                items.push_back(build_candidate_item_json(
                    h_ctx.target_id,
                    kContextRetryTargetNewEn,
                    "mistranslation",
                    "critical",
                    kBlockedCriticalConfidence,
                    "Fix benefactive inversion and leaked kanji 懐中電灯"));
            }

            std::string raw = "[";
            for (std::size_t i = 0; i < items.size(); ++i) {
                if (i > 0U) {
                    raw += ",";
                }
                raw += items[i];
            }
            raw += "]";

            LlmResponse resp;
            resp.text = std::move(raw);
            resp.finish_reason = "STOP";
            resp.prompt_tokens = kNormalWinPromptTokens;
            resp.output_tokens = kNormalWinOutputTokens;
            resp.thought_tokens = kNormalWinThoughtTokens;
            return Result<LlmResponse>::success(resp);
        });

    SpotCheckConfig run1_cfg;
    run1_cfg.game_dir = wide_to_utf8(game_dir.wstring());
    run1_cfg.mode = SpotCheckMode::Hotspot;
    run1_cfg.window_size = kSmallWindowSize;
    run1_cfg.context_before = kBlockedContextBefore;
    run1_cfg.context_after = kBlockedContextBefore;
    run1_cfg.z_threshold = kBlockedZThreshold;
    run1_cfg.radar_calls = 1U;
    run1_cfg.max_windows = kBlockedMaxWindows;
    run1_cfg.auto_apply = true;
    run1_cfg.model = std::string(kDefaultArgonModel);

    auto checker1 = SpotChecker::create(run1_cfg, deps);
    ASSERT_TRUE(checker1.is_ok()) << checker1.error_message();
    // Both Stage-1 and Stage-2 system prompts carry the localization QA framing banner.
    EXPECT_FALSE(checker1.value().system_prompt().empty());

    const auto res1 = checker1.value().run();
    ASSERT_TRUE(res1.is_ok()) << res1.error_message();
    const auto& rep1 = res1.value();

    // Refused Radar falls back to statistical hotspot order: Call 1 inspects Cluster #1 (`[0, 10)`).
    ASSERT_GE(transport.requests().size(), kMinRun1BlockedApiCalls);
    EXPECT_TRUE(
        inspect_target_in_user_prompt(transport.requests()[1].user_prompt, kHardTriggerJp)
            .found);

    // Warnings logged for both `PROHIBITED_CONTENT` and `SAFETY`.
    ASSERT_GE(logs.warns().size(), kExpectedBlockedWarnings);
    EXPECT_TRUE(contains_log_substring(logs.warns(), "PROHIBITED_CONTENT"));
    EXPECT_TRUE(contains_log_substring(logs.warns(), "SAFETY"));

    // Exact blocked-leaf tracking: `[0, 5)` recorded ONLY in `blocked_*`, NEVER in `checked_*`!
    EXPECT_EQ(rep1.radar_calls_made, 1U);
    EXPECT_GE(rep1.radar_passages_triaged, kExpectedRun1Improvements);
    EXPECT_EQ(rep1.windows_blocked, 1U);
    EXPECT_EQ(rep1.lines_blocked, kDefaultMinBlockedSplitLines);
    EXPECT_EQ(rep1.blocked_offsets, std::vector<std::size_t>{0U});
    EXPECT_EQ(rep1.blocked_spans,
              (std::vector<std::pair<std::size_t, std::size_t>>{
                  {0U, kDefaultMinBlockedSplitLines}}));
    for (const auto& span : rep1.checked_spans) {
        EXPECT_GE(span.first, kDefaultMinBlockedSplitLines);
    }

    // Both `[5, 10)` (salvaged via bisection) and Cluster #2 (salvaged via context-strip retry) repaired!
    EXPECT_GE(rep1.windows_checked, kExpectedRun1Improvements);
    EXPECT_EQ(rep1.improvements_found, kExpectedRun1Improvements);
    EXPECT_EQ(rep1.token_usage.api_calls, transport.requests().size());

    const auto ckpt1 =
        boost::json::parse(TempSandbox::read_bytes(
                               game_dir / "script_output" / "argon_spot_check.json"))
            .as_object();
    EXPECT_EQ(ckpt1.at("windows_blocked").to_number<std::size_t>(), 1U);
    EXPECT_EQ(ckpt1.at("lines_blocked").to_number<std::size_t>(),
              kDefaultMinBlockedSplitLines);
    ASSERT_EQ(ckpt1.at("blocked_spans").as_array().size(), 1U);

    const auto tt_after_run1 =
        boost::json::parse(TempSandbox::read_bytes(tt_path)).as_object();
    const auto& s01_run1 =
        tt_after_run1.at("scene_01").as_object().at("strings").as_array();
    ASSERT_EQ(s01_run1.size(), kBlockedCorpusSize);
    EXPECT_EQ(
        s01_run1[kBlockedHardTriggerIdx].as_object().at("translated").as_string(),
        kHardTriggerOldEn);
    EXPECT_EQ(
        s01_run1[kBlockedLeftCompanionIdx].as_object().at("translated").as_string(),
        kLeftCompanionOldEn);
    EXPECT_EQ(
        s01_run1[kBlockedRightSalvagedIdx].as_object().at("translated").as_string(),
        kRightSalvagedNewEn);
    EXPECT_EQ(
        s01_run1[kBlockedContextRetryTargetIdx].as_object().at("translated").as_string(),
        kContextRetryTargetNewEn);

    // Run 2 (same-model resume `"gemini-4-argon"`): skips both `checked_spans` and `blocked_spans`!
    transport.clear_requests();
    logs.clear();
    SpotCheckConfig run2_cfg = run1_cfg;
    run2_cfg.radar_calls = 0U;

    auto checker2 = SpotChecker::create(run2_cfg, deps);
    ASSERT_TRUE(checker2.is_ok()) << checker2.error_message();
    const auto res2 = checker2.value().run();
    ASSERT_TRUE(res2.is_ok()) << res2.error_message();
    EXPECT_TRUE(transport.requests().empty());
    EXPECT_EQ(res2.value().windows_blocked, 1U);
    EXPECT_EQ(res2.value().lines_blocked, kDefaultMinBlockedSplitLines);

    // Run 3 (alternate-`--model` resume): keeps `checked_spans` skipped, automatically
    // re-queues `blocked_spans` (`[0, 5)`), repairs `indices 0, 2`, and clears `blocked_spans`!
    fallback_model_active = true;
    transport.clear_requests();
    logs.clear();
    SpotCheckConfig run3_cfg = run2_cfg;
    run3_cfg.model = "gemini-4-argon-fallback";

    auto checker3 = SpotChecker::create(run3_cfg, deps);
    ASSERT_TRUE(checker3.is_ok()) << checker3.error_message();
    const auto res3 = checker3.value().run();
    ASSERT_TRUE(res3.is_ok()) << res3.error_message();
    const auto& rep3 = res3.value();

    ASSERT_GE(transport.requests().size(), 1U);
    EXPECT_TRUE(
        inspect_target_in_user_prompt(transport.requests()[0].user_prompt, kHardTriggerJp)
            .found);
    EXPECT_FALSE(
        inspect_target_in_user_prompt(transport.requests()[0].user_prompt, kRightSalvagedJp)
            .found);
    EXPECT_FALSE(
        inspect_target_in_user_prompt(transport.requests()[0].user_prompt,
                                      kContextRetryTargetJp)
            .found);

    EXPECT_EQ(rep3.windows_blocked, 0U);
    EXPECT_EQ(rep3.lines_blocked, 0U);
    EXPECT_TRUE(rep3.blocked_offsets.empty());
    EXPECT_TRUE(rep3.blocked_spans.empty());
    EXPECT_EQ(rep3.improvements_found, kExpectedTotalBlockedImprovements);

    const auto ckpt3 =
        boost::json::parse(TempSandbox::read_bytes(
                               game_dir / "script_output" / "argon_spot_check.json"))
            .as_object();
    EXPECT_EQ(ckpt3.at("model").as_string(), "gemini-4-argon-fallback");
    EXPECT_EQ(ckpt3.at("windows_blocked").to_number<std::size_t>(), 0U);
    EXPECT_EQ(ckpt3.at("lines_blocked").to_number<std::size_t>(), 0U);
    EXPECT_TRUE(ckpt3.at("blocked_spans").as_array().empty());

    const auto tt_after_run3 =
        boost::json::parse(TempSandbox::read_bytes(tt_path)).as_object();
    const auto& s01_run3 =
        tt_after_run3.at("scene_01").as_object().at("strings").as_array();
    ASSERT_EQ(s01_run3.size(), kBlockedCorpusSize);
    EXPECT_EQ(
        s01_run3[kBlockedHardTriggerIdx].as_object().at("translated").as_string(),
        kHardTriggerNewEn);
    EXPECT_EQ(
        s01_run3[kBlockedLeftCompanionIdx].as_object().at("translated").as_string(),
        kLeftCompanionNewEn);
    EXPECT_EQ(
        s01_run3[kBlockedRightSalvagedIdx].as_object().at("translated").as_string(),
        kRightSalvagedNewEn);
    EXPECT_EQ(
        s01_run3[kBlockedContextRetryTargetIdx].as_object().at("translated").as_string(),
        kContextRetryTargetNewEn);
    EXPECT_FALSE(has_any_tmp_files(game_dir));
}

TEST(SpotCheckerComponentTest,
     SustainedHSceneAvoidanceClipsContextAndContagionPreservesDirtyBanterAndCombatAndUnlocksOnOptIn) {
    TempSandbox sandbox("ct_sustained_h_avoidance");
    ScriptedLlmTransport transport;
    FaultInjectingFileStore store;
    RecordingCryptSync crypt;
    FakeClock clock;
    CapturingLogSink logs;
    const auto deps = bind_deps(transport, store, crypt, clock, logs);

    const auto game_dir = sandbox.path() / "NovelMatureBoundary";

    // Build `scene_01` (92 lines):
    // - `[0, 10)`: normal opening with 3 defects (`2, 5, 9`) right against `[10, 36)`
    //   so both `context_after = 10` and right-neighbor contagion try to enter `[10, 36)`.
    // - `[10, 36)` (26 lines, NO `(Ｈ)` tags): sustained pure vocalization/moan/wet-SFX H-scene.
    // - `[36, 64)` (28 lines): normal comedy scene with a 4-line explicit dirty-banter burst `[44, 48)`.
    // - `[64, 92)` (28 lines): sustained combat/injury-pain/breathless-chase scene (`ぐあっ`/`くっ`/`はぁ、はぁ`).
    std::vector<SyntheticRow> scene01;
    scene01.reserve(kSustainedCtScene01Lines);
    for (std::size_t i = 0; i < kSustainedCtScene01Lines; ++i) {
        const auto& base = kCleanBaselinePool[i % kCleanBaselinePool.size()];
        scene01.push_back(
            {std::string(base.speaker_jp), std::string(base.jp), std::string(base.en), false});
    }

    constexpr std::string_view kOpenDefectAJp =
        "「古い時計台の階段は雨の日に滑りやすいから気をつけろ」";
    constexpr std::string_view kOpenDefectAOldEn =
        "\"Watch your step because the old 時計台 stairs get slick in the rain.\"";
    constexpr std::string_view kOpenDefectANewEn =
        "\"Watch your step because the old clock tower stairs get slippery in the rain.\"";

    constexpr std::string_view kOpenDefectBJp =
        "「先輩に予備の懐中電灯を貸してもらえて助かったよ」";
    constexpr std::string_view kOpenDefectBOldEn =
        "\"I lent my (senpai) a spare 懐中電灯 and it saved me.\"";
    constexpr std::string_view kOpenDefectBNewEn =
        "\"My senior lent me a spare flashlight, and it really helped me out.\"";

    constexpr std::string_view kOpenDefectEdgeJp =
        "「廊下の奥にある配電盤の鍵を閉め忘れないでね」";
    constexpr std::string_view kOpenDefectEdgeOldEn =
        "\"Don't forget to lock the 配電盤の鍵 at the end of the hall.\"";
    constexpr std::string_view kOpenDefectEdgeNewEn =
        "\"Don't forget to lock the breaker panel at the end of the hallway.\"";

    scene01[kSustainedCtOpenDefectA] = {
        "高瀬 蓮", std::string(kOpenDefectAJp), std::string(kOpenDefectAOldEn), false};
    scene01[kSustainedCtOpenDefectB] = {
        "蒼井 遥", std::string(kOpenDefectBJp), std::string(kOpenDefectBOldEn), false};
    scene01[kSustainedCtOpenDefectEdge] = {
        "蒼井 遥", std::string(kOpenDefectEdgeJp), std::string(kOpenDefectEdgeOldEn), false};

    constexpr std::array<BaselinePair, 6> kPureVocalCues{{
        {"蒼井 遥",
         "「んっ……あっ、はぁっ……そんなふうに耳元で息をされたら……♡」",
         "\"Nngh... aah, haaah... if you breathe right against my ear like that...\""},
        {"",
         "んちゅ、ちゅぱ……じゅるるっ、と重なり合った唇から甘い水音が静かな部屋に響く。",
         "*Mmph*, *slurp*... wet kissing sounds echoed softly through the quiet room."},
        {"蒼井 遥",
         "「ふぁぁっ……んぅ、くぅぅっ……やぁ、声が勝手に漏れて……っ♪」",
         "\"Fwaaah... nngh, kuhhh... n-no, my voice keeps slipping out on its own...!\""},
        {"",
         "ぬちゅ、くちゅ……ぴちゃっ、と絡みつくような粘り気のある音が何度も繰り返された。",
         "*Squelch*, *schlick*... sticky, clinging wet sounds repeated again and again."},
        {"蒼井 遥",
         "「あっ、あっ、あんっ……！ひゃんっ、そこばかり強くしないでぇ……♡」",
         "\"Ah, ah, aghhhhh...! Hyan, don't press right there so hard...!\""},
        {"",
         "びゅるっ、びゅるるっ……と熱い震えが伝わり、二人の荒い吐息がいつまでも重なり合っていた。",
         "*Splurt*, *throb*... hot shudders rippled through them as their ragged breaths intertwined."},
    }};
    for (std::size_t i = kSustainedCtVocalHBegin; i < kSustainedCtVocalHEnd; ++i) {
        const auto& cue = kPureVocalCues[(i - kSustainedCtVocalHBegin) % kPureVocalCues.size()];
        scene01[i] = {
            std::string(cue.speaker_jp), std::string(cue.jp), std::string(cue.en), false};
    }

    constexpr std::string_view kVocalHDefectJp =
        "「んっ……あっ、はぁっ……耳元で熱く囁かれたら声が止まらないよぉ……♡」";
    constexpr std::string_view kVocalHDefectOldEn =
        "\"Nngh... aghhhhh, haaah... 耳元で囁かれたら (TL: moan) my voice won't stop...!\"";
    constexpr std::string_view kVocalHDefectNewEn =
        "\"Nngh... aghhhhh, haaah... if you whisper so warmly in my ear, I can't hold my voice back...!\"";
    scene01[kSustainedCtVocalDefectIdx] = {
        "蒼井 遥", std::string(kVocalHDefectJp), std::string(kVocalHDefectOldEn), false};

    // 4-line explicit dirty-banter burst (`indices 44..47`) inside normal comedy dialogue.
    constexpr std::string_view kBanterLine0Jp =
        "「お前、部室のロッカーにエロ本とコンドーム隠してたのがバレたんだって？」";
    constexpr std::string_view kBanterLine0En =
        "\"Didn't you get busted hiding porn mags and condoms in the clubroom locker?\"";
    constexpr std::string_view kBanterDefectJp =
        "「朝から童貞だの勃起だの下品な冗談ばかり言わないでよ！」";
    constexpr std::string_view kBanterDefectOldEn =
        "\"Quit cracking crude jokes about being a virgin or getting an erection in the 教室 (senpai)!\"";
    constexpr std::string_view kBanterDefectNewEn =
        "\"Quit cracking crude jokes about being a virgin or getting an erection in the classroom!\"";
    constexpr std::string_view kBanterLine2Jp =
        "「いや、あいつが勝手にセックスだのおっぱいだの騒ぎ出したんだぞ」";
    constexpr std::string_view kBanterLine2En =
        "\"Look, he's the idiot who started yelling about sex and boobs in the hallway!\"";
    constexpr std::string_view kBanterLine3Jp =
        "「どっちも同罪だよ、変態扱いされたくなかったら口を慎みなさい」";
    constexpr std::string_view kBanterLine3En =
        "\"You're both guilty—watch your mouth unless you want everyone calling you a pervert.\"";

    scene01[kSustainedCtBanterBegin] = {
        "高瀬 蓮", std::string(kBanterLine0Jp), std::string(kBanterLine0En), false};
    scene01[kSustainedCtBanterDefectIdx] = {
        "蒼井 遥", std::string(kBanterDefectJp), std::string(kBanterDefectOldEn), false};
    scene01[kSustainedCtBanterBegin + 2U] = {
        "高瀬 蓮", std::string(kBanterLine2Jp), std::string(kBanterLine2En), false};
    scene01[kSustainedCtBanterBegin + 3U] = {
        "蒼井 遥", std::string(kBanterLine3Jp), std::string(kBanterLine3En), false};

    // 28-line sustained combat/pain/breathless-chase stretch (`indices 64..91`).
    constexpr std::array<BaselinePair, 4> kCombatCues{{
        {"高瀬 蓮",
         "「はぁ、はぁ……止まるな、非常階段を抜ければ屋上へ出られるぞ！」",
         "\"Huff, huff... don't stop! Once we clear the fire stairs we can reach the roof!\""},
        {"高瀬 蓮",
         "「ぐあっ……！くそっ、脇腹を刃で浅く斬られただけだ、まだ走れる！」",
         "\"Gah...! Damn it, the blade only grazed my side—I can still run!\""},
        {"蒼井 遥",
         "「あっ……袖から血が滲んでるよ！早く傷口を押さえないと……！」",
         "\"Ah... blood is soaking through your sleeve! Press down on the wound fast...!\""},
        {"高瀬 蓮",
         "「くっ……うわあああっ！この鉄扉さえ封鎖すれば敵は入ってこられない！」",
         "\"Kuh... uwaaaah! Once we barricade this iron door, the enemy won't get through!\""},
    }};
    for (std::size_t i = kSustainedCtCombatBegin; i < kSustainedCtScene01Lines; ++i) {
        const auto& c = kCombatCues[(i - kSustainedCtCombatBegin) % kCombatCues.size()];
        scene01[i] = {std::string(c.speaker_jp), std::string(c.jp), std::string(c.en), false};
    }

    constexpr std::string_view kCombatDefectJp =
        "「ぐあっ……！敵の剣が肩を掠めたが、屋上の扉まで走り抜けるぞ！」";
    constexpr std::string_view kCombatDefectOldEn =
        "\"Gah...! The enemy's 剣 grazed my shoulder, but sprint to the 屋上の扉!\"";
    constexpr std::string_view kCombatDefectNewEn =
        "\"Gah...! The enemy's sword grazed my shoulder, but let's sprint to the rooftop door!\"";
    scene01[kSustainedCtCombatDefectIdx] = {
        "高瀬 蓮", std::string(kCombatDefectJp), std::string(kCombatDefectOldEn), false};

    // Build `scene_02` (24 lines, `global_index 92..115`): untagged interleaved
    // dialogue + literary eroge narration + 1-line plain emotional bridge sentences.
    constexpr std::array<BaselinePair, 4> kLiteraryCycle{{
        {"",
         "「んっ……あぁっ、そんな奥まで一気に来られたら……っ♡」",
         "\"Nngh... aaah, if you push all the way deep inside at once...!\""},
        {"",
         "濡れそぼった秘裂が熱い肉棒を根元まで咥え込み、愛液がシーツを濃く染めていく。",
         "Her drenched slit swallowed his hot shaft to the hilt as slick love juices darkened the sheets."},
        {"",
         "窓の外を叩く雨音さえ遠ざかり、頭の中が真っ白な熱に溶けていくようだった。",
         "Even the sound of rain against the window faded away, her thoughts dissolving into white heat."},
        {"",
         "激しい抽挿の果てに柔らかな蜜壺の肉襞が震え、濃厚な白濁の精液が奥深くを満たした。",
         "At the peak of his fierce thrusting the inner walls of her honeyed folds trembled as thick white semen filled her core."},
    }};
    std::vector<SyntheticRow> scene02;
    scene02.reserve(kSustainedCtScene02Lines);
    for (std::size_t i = 0; i < kSustainedCtScene02Lines; ++i) {
        const auto& c = kLiteraryCycle[i % kLiteraryCycle.size()];
        scene02.push_back({"", std::string(c.jp), std::string(c.en), false});
    }

    constexpr std::string_view kLiteraryHDefectJp =
        "濡れそぼった秘裂が熱く震え、濃厚な白濁が蜜壺の奥深くを満たしていった。";
    constexpr std::string_view kLiteraryHDefectOldEn =
        "Her drenched 秘裂 trembled hotly as thick 白濁 filled her 蜜壺 (TL: eroge).";
    constexpr std::string_view kLiteraryHDefectNewEn =
        "Her drenched folds trembled with heat as thick white release filled her deepest core.";
    scene02[kSustainedCtLiteraryLocalDefectIdx] = {
        "", std::string(kLiteraryHDefectJp), std::string(kLiteraryHDefectOldEn), false};

    const auto tt_path = sandbox.write_bytes(
        "NovelMatureBoundary/script_output/translated_text.json",
        serialize_crlf_indent1_corpus(scene01, scene02));

    bool saw_banter_editable = false;
    bool saw_combat_editable = false;
    bool saw_vocal_h_editable = false;
    bool saw_literary_h_editable = false;

    transport.set_default_handler(
        [&](const LlmRequest& req, std::size_t) -> Result<LlmResponse> {
            std::vector<std::string> items;

            const auto open_a =
                inspect_target_in_user_prompt(req.user_prompt, kOpenDefectAJp);
            if (open_a.found && open_a.editable) {
                items.push_back(build_candidate_item_json(
                    open_a.target_id,
                    kOpenDefectANewEn,
                    "mistranslation",
                    "major",
                    kBlockedMajorConfidence,
                    "Translate leaked kanji 時計台"));
            }
            const auto open_b =
                inspect_target_in_user_prompt(req.user_prompt, kOpenDefectBJp);
            if (open_b.found && open_b.editable) {
                items.push_back(build_candidate_item_json(
                    open_b.target_id,
                    kOpenDefectBNewEn,
                    "mistranslation",
                    "critical",
                    kBlockedCriticalConfidence,
                    "Fix benefactive inversion, romaji note, and leaked kanji"));
            }
            const auto open_edge =
                inspect_target_in_user_prompt(req.user_prompt, kOpenDefectEdgeJp);
            if (open_edge.found && open_edge.editable) {
                items.push_back(build_candidate_item_json(
                    open_edge.target_id,
                    kOpenDefectEdgeNewEn,
                    "mistranslation",
                    "major",
                    kBlockedMajorConfidence,
                    "Translate leaked kanji 配電盤の鍵"));
            }
            const auto banter =
                inspect_target_in_user_prompt(req.user_prompt, kBanterDefectJp);
            if (banter.found && banter.editable) {
                saw_banter_editable = true;
                items.push_back(build_candidate_item_json(
                    banter.target_id,
                    kBanterDefectNewEn,
                    "artifact_or_note",
                    "major",
                    kBlockedMajorConfidence,
                    "Translate leaked kanji and remove romaji note in dirty banter"));
            }
            const auto combat =
                inspect_target_in_user_prompt(req.user_prompt, kCombatDefectJp);
            if (combat.found && combat.editable) {
                saw_combat_editable = true;
                items.push_back(build_candidate_item_json(
                    combat.target_id,
                    kCombatDefectNewEn,
                    "mistranslation",
                    "major",
                    kBlockedMajorConfidence,
                    "Translate leaked kanji in combat scene"));
            }
            const auto vocal_h =
                inspect_target_in_user_prompt(req.user_prompt, kVocalHDefectJp);
            if (vocal_h.found && vocal_h.editable) {
                saw_vocal_h_editable = true;
                items.push_back(build_candidate_item_json(
                    vocal_h.target_id,
                    kVocalHDefectNewEn,
                    "artifact_or_note",
                    "major",
                    kBlockedMajorConfidence,
                    "Translate leaked kanji and remove TL note in opt-in H-scene"));
            }
            const auto literary_h = inspect_target_in_user_prompt(
                req.user_prompt, kLiteraryHDefectJp);
            if (literary_h.found && literary_h.editable) {
                saw_literary_h_editable = true;
                items.push_back(build_candidate_item_json(
                    literary_h.target_id,
                    kLiteraryHDefectNewEn,
                    "artifact_or_note",
                    "major",
                    kBlockedMajorConfidence,
                    "Translate leaked literary eroge kanji and remove TL note"));
            }

            std::string raw = "[";
            for (std::size_t i = 0; i < items.size(); ++i) {
                if (i > 0U) {
                    raw += ",";
                }
                raw += items[i];
            }
            raw += "]";

            LlmResponse resp;
            resp.text = std::move(raw);
            resp.finish_reason = "STOP";
            resp.prompt_tokens = kNormalWinPromptTokens;
            resp.output_tokens = kNormalWinOutputTokens;
            resp.thought_tokens = kNormalWinThoughtTokens;
            return Result<LlmResponse>::success(resp);
        });

    // Phase 1: Default Hotspot run (`skip_h_scenes = true`, `auto_apply = false`).
    SpotCheckConfig default_cfg;
    default_cfg.game_dir = wide_to_utf8(game_dir.wstring());
    default_cfg.mode = SpotCheckMode::Hotspot;
    default_cfg.window_size = kSmallWindowSize;
    default_cfg.context_before = kSmallWindowSize;
    default_cfg.context_after = kSmallWindowSize;
    default_cfg.z_threshold = 0.0;
    default_cfg.radar_calls = 0U;
    default_cfg.skip_h_scenes = true;
    default_cfg.auto_apply = false;
    default_cfg.out_path =
        wide_to_utf8((sandbox.path() / "phase1_default.json").wstring());

    auto checker1 = SpotChecker::create(default_cfg, deps);
    ASSERT_TRUE(checker1.is_ok()) << checker1.error_message();
    const auto res1 = checker1.value().run();
    ASSERT_TRUE(res1.is_ok()) << res1.error_message();
    const auto& rep1 = res1.value();

    // Neither `[10, 36)` (pure moan H-scene) nor `[92, 116)` (interleaved literary H-scene)
    // is ever entered via hotspot selection, `<context_before>`/`<context_after>`, or contagion!
    for (const auto& req : transport.requests()) {
        EXPECT_EQ(req.user_prompt.find("耳元で息をされたら"), std::string::npos);
        EXPECT_EQ(req.user_prompt.find(kVocalHDefectJp), std::string::npos);
        EXPECT_EQ(req.user_prompt.find("濡れそぼった秘裂"), std::string::npos);
        EXPECT_EQ(req.user_prompt.find(kLiteraryHDefectJp), std::string::npos);
        EXPECT_EQ(req.user_prompt.find("[REDACTED EXPLICIT SCENE LINE]"),
                  std::string::npos);
    }

    // Dirty banter (`[44, 48)`) AND 28-line combat panting (`[64, 92)`) ARE sent verbatim and repaired!
    EXPECT_TRUE(saw_banter_editable);
    EXPECT_TRUE(saw_combat_editable);
    EXPECT_FALSE(saw_vocal_h_editable);
    EXPECT_FALSE(saw_literary_h_editable);
    EXPECT_EQ(rep1.improvements_found, kSustainedCtExpectedDefaultImprovements);

    const bool saw_all_banter_lines_verbatim = std::any_of(
        transport.requests().begin(),
        transport.requests().end(),
        [&](const LlmRequest& req) {
            return req.user_prompt.find(kBanterDefectJp) != std::string::npos &&
                   req.user_prompt.find(kBanterLine0Jp) != std::string::npos &&
                   req.user_prompt.find(kBanterLine2Jp) != std::string::npos &&
                   req.user_prompt.find(kBanterLine3Jp) != std::string::npos;
        });
    EXPECT_TRUE(saw_all_banter_lines_verbatim);

    // Phase 2: Default Linear run across `[5, 35)` (`offset = 5, window = 10, max_windows = 3`):
    // - `[5, 15)` trims to `[5, 10)` (`5` lines checked) with `<context_after>` clipped at `10`.
    // - `[15, 25)` and `[25, 35)` lie inside `[10, 36)` and increment `h_windows_skipped`.
    transport.clear_requests();
    SpotCheckConfig linear_cfg = default_cfg;
    linear_cfg.mode = SpotCheckMode::Linear;
    linear_cfg.offset = kSustainedCtLinearOffset;
    linear_cfg.max_windows = kSustainedCtLinearMaxWindows;
    linear_cfg.out_path =
        wide_to_utf8((sandbox.path() / "phase2_linear.json").wstring());

    auto checker2 = SpotChecker::create(linear_cfg, deps);
    ASSERT_TRUE(checker2.is_ok()) << checker2.error_message();
    const auto res2 = checker2.value().run();
    ASSERT_TRUE(res2.is_ok()) << res2.error_message();
    const auto& rep2 = res2.value();

    EXPECT_EQ(rep2.windows_checked, 1U);
    EXPECT_EQ(rep2.lines_checked, kSustainedCtExpectedTrimmedLines);
    EXPECT_GE(rep2.h_windows_skipped, kSustainedCtMinSkippedLinearHWindows);
    for (const auto& req : transport.requests()) {
        EXPECT_EQ(req.user_prompt.find("耳元で息をされたら"), std::string::npos);
        EXPECT_EQ(req.user_prompt.find(kVocalHDefectJp), std::string::npos);
    }

    // Phase 3: Explicit opt-in (`skip_h_scenes = false` / `--include-h-scenes`, `auto_apply = true`):
    // unlocks both sustained H-scenes (`[10, 36)` and `[92, 116)`) and writes all 7 fixes!
    transport.clear_requests();
    SpotCheckConfig optin_cfg = default_cfg;
    optin_cfg.skip_h_scenes = false;
    optin_cfg.auto_apply = true;
    optin_cfg.out_path =
        wide_to_utf8((sandbox.path() / "phase3_optin.json").wstring());

    auto checker3 = SpotChecker::create(optin_cfg, deps);
    ASSERT_TRUE(checker3.is_ok()) << checker3.error_message();
    const auto res3 = checker3.value().run();
    ASSERT_TRUE(res3.is_ok()) << res3.error_message();
    const auto& rep3 = res3.value();

    EXPECT_TRUE(saw_vocal_h_editable);
    EXPECT_TRUE(saw_literary_h_editable);
    EXPECT_EQ(rep3.h_windows_skipped, 0U);
    EXPECT_EQ(rep3.improvements_found, kSustainedCtExpectedOptInImprovements);

    const auto updated_tt =
        boost::json::parse(TempSandbox::read_bytes(tt_path)).as_object();
    const auto& s01_arr =
        updated_tt.at("scene_01").as_object().at("strings").as_array();
    const auto& s02_arr =
        updated_tt.at("scene_02").as_object().at("lines").as_array();
    ASSERT_EQ(s01_arr.size(), kSustainedCtScene01Lines);
    ASSERT_EQ(s02_arr.size(), kSustainedCtScene02Lines);
    EXPECT_EQ(
        s01_arr[kSustainedCtVocalDefectIdx].as_object().at("translated").as_string(),
        kVocalHDefectNewEn);
    EXPECT_EQ(
        s01_arr[kSustainedCtBanterDefectIdx].as_object().at("translated").as_string(),
        kBanterDefectNewEn);
    EXPECT_EQ(
        s01_arr[kSustainedCtCombatDefectIdx].as_object().at("translated").as_string(),
        kCombatDefectNewEn);
    EXPECT_EQ(s02_arr[kSustainedCtLiteraryLocalDefectIdx]
                  .as_object()
                  .at("translated")
                  .as_string(),
              kLiteraryHDefectNewEn);
    EXPECT_FALSE(has_any_tmp_files(game_dir));
}

}  // namespace
}  // namespace spot_checker
