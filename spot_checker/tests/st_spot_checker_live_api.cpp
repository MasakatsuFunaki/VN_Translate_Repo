// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "checker/spot_checker.h"
#include "client/argon_client.h"
#include "common/util.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>

namespace bj = boost::json;
namespace fs = std::filesystem;
using namespace spot_checker;

namespace {

constexpr std::size_t kRunawayRepeatCount = 1200;
constexpr std::size_t kTestWindowSize = 10;
constexpr std::size_t kSingleWindowCount = 1;
constexpr double kZeroZThreshold = 0.0;
constexpr std::size_t kExpectedUnlockedLines = 6;
constexpr std::size_t kMinExpectedImprovements = 4;
constexpr std::size_t kMaxCompressedScreamLength = 200;

constexpr std::size_t kCleanDialogueIndex = 0;
constexpr std::size_t kSymbolPassthroughIndex = 1;
constexpr std::size_t kHeaderLineIndex = 2;
constexpr std::size_t kMistranslationIndex = 3;
constexpr std::size_t kMisgenderedIndex = 4;
constexpr std::size_t kInvertedNarrativeIndex = 5;
constexpr std::size_t kRunawayScreamIndex = 6;

class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        path_ = fs::temp_directory_path() /
                ("vn_spot_checker_st_" + std::to_string(++counter));
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_ / "script_output", ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    std::string str() const { return path_.u8string(); }
    fs::path path() const { return path_; }

private:
    fs::path path_;
};

}  // namespace

TEST(SpotCheckerSystemTests, SystemTest12_LiveApiEndToEndOnSyntheticCorruptedNovel) {
    const wchar_t* live_gate = _wgetenv(L"VN_RUN_LIVE_API_TESTS");
    if (live_gate == nullptr || std::wstring_view(live_gate) != L"1") {
        GTEST_SKIP() << "VN_RUN_LIVE_API_TESTS!=1; skipping live LLM API system test";
    }

    const std::string api_key = load_gemini_api_key();
    if (api_key.empty()) {
        GTEST_SKIP() << "Skipping live API test: GEMINI_API_KEY not configured";
    }

    TempDir tmp;
    write_file((tmp.path() / "TONE.md").u8string(),
               "# Characters\n"
               "- Ren (レン): Calm male protagonist.\n"
               "- Miyaka (美弥香): Gentle heroine.\n"
               "- Kyou (狂): Dangerous female antagonist. Always use she/her pronouns.\n");

    const std::string runaway_scream =
        "Miyaka\r\n\"A" + std::string(kRunawayRepeatCount, 'a') + "\r\n";

    const bj::array strings_arr = {
        bj::object{
            {"index", static_cast<std::int64_t>(kCleanDialogueIndex)},
            {"type", "dialogue"},
            {"text", "レン\r\n「静かな夜ですね。少し休みましょうか」\r\n"},
            {"translated", "Ren\r\n\"It's a quiet night. Shall we rest for a bit?\"\r\n"},
        },
        bj::object{
            {"index", static_cast<std::int64_t>(kSymbolPassthroughIndex)},
            {"type", "dialogue"},
            {"text", "美弥香\r\n「？」\r\n"},
            {"translated", "Miyaka\r\n「？」\r\n"},
        },
        bj::object{
            {"index", static_cast<std::int64_t>(kHeaderLineIndex)},
            {"type", "dialogue"},
            {"text", ".■ウインドウ表示■\r\n"},
            {"translated", "■ Window Display ■\r\n"},
        },
        bj::object{
            {"index", static_cast<std::int64_t>(kMistranslationIndex)},
            {"type", "dialogue"},
            {"text", "レン\r\n「父はどこですか？」\r\n"},
            {"translated", "Ren\r\n\"Where is the hospital?\"\r\n"},
        },
        bj::object{
            {"index", static_cast<std::int64_t>(kMisgenderedIndex)},
            {"type", "narrative"},
            {"text", "狂は不気味に笑いながら、右手に蟲筒を握り締めていた。\r\n"},
            {"translated", "Smiling eerily, he gripped a Bug tube in his right hand.\r\n"},
        },
        bj::object{
            {"index", static_cast<std::int64_t>(kInvertedNarrativeIndex)},
            {"type", "narrative"},
            {"text", "たとえ待っていても、\r\n誰も助けには来なかった。\r\n"},
            {"translated", "Even if you wait,\r\neveryone came to help.\r\n"},
        },
        bj::object{
            {"index", static_cast<std::int64_t>(kRunawayScreamIndex)},
            {"type", "dialogue"},
            {"text", "美弥香\r\n「ああああああああああああああああああああッッ！！！」\r\n"},
            {"translated", runaway_scream},
        },
    };

    const std::string translated_path =
        (tmp.path() / "script_output" / "translated_text.json").u8string();
    write_file(translated_path,
               json_pretty(bj::value(bj::object{
                               {"s100.spt", bj::object{{"strings", strings_arr}}}}),
                           kDefaultJsonPrettyIndent));

    SpotCheckConfig cfg;
    cfg.game_dir = tmp.str();
    cfg.mode = SpotCheckMode::Hotspot;
    cfg.window_size = kTestWindowSize;
    cfg.max_windows = kSingleWindowCount;
    cfg.z_threshold = kZeroZThreshold;
    cfg.radar_calls = 0;

    EXPECT_TRUE(cfg.review_all_in_window);

    SpotChecker live_checker(cfg);
    SpotCheckReport rep;
    try {
        rep = live_checker.run();
    } catch (const std::exception& ex) {
        const std::string msg = ex.what();
        if (msg.find("HTTP 429") != std::string::npos ||
            msg.find("RESOURCE_EXHAUSTED") != std::string::npos) {
            GTEST_SKIP() << "Skipping live API test due to API quota/spend cap: "
                         << msg;
        }
        throw;
    }

    EXPECT_EQ(rep.windows_checked, kSingleWindowCount);
    EXPECT_EQ(rep.editable_lines_unlocked, kExpectedUnlockedLines);
    EXPECT_GE(rep.improvements_found, kMinExpectedImprovements);

    const bj::value updated_root = json_parse_file(translated_path);
    const auto& out_arr =
        updated_root.as_object().at("s100.spt").as_object().at("strings").as_array();

    EXPECT_EQ(out_arr[kCleanDialogueIndex].as_object().at("translated").as_string(),
              "Ren\r\n\"It's a quiet night. Shall we rest for a bit?\"\r\n");
    EXPECT_EQ(out_arr[kSymbolPassthroughIndex].as_object().at("translated").as_string(),
              "Miyaka\r\n「？」\r\n");
    EXPECT_EQ(out_arr[kHeaderLineIndex].as_object().at("translated").as_string(),
              "■ Window Display ■\r\n");

    const std::string fixed_3(
        out_arr[kMistranslationIndex].as_object().at("translated").as_string());
    EXPECT_EQ(fixed_3.rfind("Ren\r\n\"", 0), 0u) << "Fixed 3: " << fixed_3;
    EXPECT_EQ(fixed_3.find("hospital"), std::string::npos) << "Fixed 3: " << fixed_3;

    const std::string fixed_4(
        out_arr[kMisgenderedIndex].as_object().at("translated").as_string());
    EXPECT_EQ(fixed_4.find(" he "), std::string::npos) << "Fixed 4: " << fixed_4;
    EXPECT_EQ(fixed_4.find(" his "), std::string::npos) << "Fixed 4: " << fixed_4;
    EXPECT_TRUE(fixed_4.find("she") != std::string::npos ||
                fixed_4.find("She") != std::string::npos ||
                fixed_4.find("her") != std::string::npos ||
                fixed_4.find("Her") != std::string::npos)
        << "Fixed 4: " << fixed_4;

    const std::string fixed_5(
        out_arr[kInvertedNarrativeIndex].as_object().at("translated").as_string());
    EXPECT_EQ(fixed_5.find("everyone"), std::string::npos) << "Fixed 5: " << fixed_5;
    EXPECT_EQ(fixed_5.find("Even if you wait,\r\nEven if"), std::string::npos)
        << "Fixed 5 duplicated first line: " << fixed_5;

    const std::string fixed_6(
        out_arr[kRunawayScreamIndex].as_object().at("translated").as_string());
    EXPECT_EQ(fixed_6.rfind("Miyaka\r\n\"", 0), 0u) << "Fixed 6: " << fixed_6;
    EXPECT_LT(fixed_6.size(), kMaxCompressedScreamLength)
        << "Fixed 6 length: " << fixed_6.size();
}
