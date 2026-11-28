// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Tier-2 game-specific live Gemini Carbon (`gemini-4-carbon`, thinkingLevel=MEDIUM)
// SYSTEM test executing the compiled 02_build_tsv binary inside an isolated
// ScopedTempDir (never touching repo script_output/) on authentic
// EXTRAVAGANZA_05_ANZU_CE BLACKCyc .spt lines, glossary speakers (夢美, アゲハ, 杏子),
// Speaker\r\n nameplates, CP932 TSV compilation, proxy_dll runtime lookup parity,
// and 0-API-call 2nd-pass resume idempotency.
#include "apps/paths.h"
#include "cache/cache_core.h"
#include "cache/gemini_client.h"
#include "cache/glossary.h"
#include "common/util.h"
#include "translator_logic.h"
#include "vn/llm/llm_client.h"
#include "vn/llm/llm_types.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace bj = boost::json;
using namespace exm;

constexpr int kMaxWalkLevels = 6;
constexpr int kLiveMaxBatches = 1;
constexpr std::int64_t kExpectedLiveLineCount = 5;
constexpr std::size_t kCrlfLen = 2;

constexpr const char* kNarrTownJp =
    "俺の町、真鈴ヶ崎にはちょっと変わった人気の喫茶店がある。\r\n";
constexpr const char* kNarrCafeAnzuJp =
    "カフェ・ＡｎｚＵという名前なんだが、他にはない\r\n"
    "サービスが満載で、今、話題沸騰中の喫茶店だ。\r\n";
constexpr const char* kDlgYumemiJp =
    "夢美\r\n「いらっしゃいませ～！　カフェ・ＡｎｚＵへようこそ！」\r\n";
constexpr const char* kDlgAgehaJp =
    "アゲハ\r\n「お客様、お一人様でしょうか？　奥の席へどうぞ」\r\n";
constexpr const char* kDlgAnzuJp =
    "杏子\r\n「ふふ、今日も夢美とアゲハのおかげでお店は大盛況ね」\r\n";

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view name)
        : path_(fs::temp_directory_path() / fs::u8path(name)) {
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

bool AsciiContainsIgnoreCase(std::string_view haystack, std::string_view needle) noexcept {
    if (needle.empty()) {
        return true;
    }
    if (haystack.size() < needle.size()) {
        return false;
    }
    const std::size_t limit = haystack.size() - needle.size();
    for (std::size_t i = 0; i <= limit; ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
            const auto a = static_cast<unsigned char>(haystack[i + j]);
            const auto b = static_cast<unsigned char>(needle[j]);
            if (std::tolower(a) != std::tolower(b)) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

bool EndsWithCrlf(std::string_view s) noexcept {
    return s.size() >= kCrlfLen &&
           s.substr(s.size() - kCrlfLen) == "\r\n";
}

fs::path ResolveRepoProjectDir() {
    fs::path cur = fs::current_path();
    for (int i = 0; i < kMaxWalkLevels; ++i) {
        if (fs::is_regular_file(cur / "build.py") &&
            fs::is_directory(cur / "pipeline_cpp")) {
            return cur;
        }
        if (fs::is_regular_file(cur / "EXTRAVAGANZA_05_ANZU_CE" / "build.py")) {
            return cur / "EXTRAVAGANZA_05_ANZU_CE";
        }
        if (!cur.has_parent_path() || cur.parent_path() == cur) {
            break;
        }
        cur = cur.parent_path();
    }
    return {};
}

fs::path ResolveBuildTsvBinary(const fs::path& repo_project_dir) {
    const fs::path test_exe_dir = fs::u8path(apps::exe_dir());
    std::vector<fs::path> candidates = {
        test_exe_dir / "02_build_tsv.exe",
        test_exe_dir.parent_path().parent_path() / "Release" / "02_build_tsv.exe",
        test_exe_dir.parent_path() / "02_build_tsv.exe",
    };
    if (!repo_project_dir.empty()) {
        candidates.push_back(repo_project_dir / "build" / "install" / "bin" / "02_build_tsv.exe");
        candidates.push_back(repo_project_dir / "build" / "pipeline" / "Release" / "02_build_tsv.exe");
    }
    for (const fs::path& cand : candidates) {
        std::error_code ec;
        if (fs::is_regular_file(cand, ec)) {
            return fs::weakly_canonical(cand, ec);
        }
    }
    return {};
}

bj::object MakeExtractedStringEntry(std::string_view type, std::string_view text) {
    bj::object o;
    o["type"] = std::string(type);
    o["text"] = std::string(text);
    o["has_jp"] = true;
    return o;
}

}  // namespace

TEST(LiveGeminiCarbonSystem, TranslatesAnzUCeScriptWithSpeakerPlatesCp932AndZeroCallResume) {
    const fs::path repo_project_dir = ResolveRepoProjectDir();
    if (!repo_project_dir.empty()) {
        gemini::load_api_key(repo_project_dir.u8string());
    }

    const bool has_gemini_key =
        apps::wide_env_utf8(L"GEMINI_API_KEY").has_value() ||
        !vn::llm::resolve_gemini_api_key().empty();
    if (!has_gemini_key) {
        const auto gh_actions = apps::wide_env_utf8(L"GITHUB_ACTIONS");
        if (gh_actions.has_value() && *gh_actions == "true") {
            GTEST_SKIP() << "GEMINI_API_KEY is unset on GitHub Actions; skipping live "
                            "gemini-4-carbon SYSTEM test.";
        }
        FAIL() << "GEMINI_API_KEY is not set (checked environment, ~/.gemini/.env, "
                  "HKCU\\Environment, and settings.json).";
    }

    const fs::path build_tsv_exe = ResolveBuildTsvBinary(repo_project_dir);
    ASSERT_FALSE(build_tsv_exe.empty())
        << "Compiled 02_build_tsv.exe not found next to test binary or in build/pipeline/Release.";

    const ScopedTempDir tmp("anzu_ce_st_gemini_carbon");
    const fs::path staged_project = tmp.path() / "project";
    const fs::path staged_script_out = staged_project / "script_output";
    const fs::path staged_game = tmp.path() / "game";
    fs::create_directories(staged_script_out);
    fs::create_directories(staged_game);

    const std::vector<std::pair<std::string, std::string>> precached_before = {
        {"老婆Ａ\r\n「ここはいつも賑やかだねぇ」\r\n",
         "Old Woman A\r\n\"This place is always so lively.\"\r\n"},
        {"老婆Ｂ\r\n「若い娘さんたちが頑張ってるからねぇ」\r\n",
         "Old Woman B\r\n\"That's because the young girls work so hard.\"\r\n"},
        {"老人\r\n「真鈴ヶ崎の名物喫茶じゃな」\r\n",
         "Old Man\r\n\"It's Masuzugasaki's famous coffee shop.\"\r\n"},
        {"通行人Ａ\r\n「カフェ・ＡｎｚＵの前に行列ができてるぞ」\r\n",
         "Passerby A\r\n\"There's a line forming outside Cafe AnzU.\"\r\n"},
        {"通行人Ｂ\r\n「俺も一度入ってみようかな」\r\n",
         "Passerby B\r\n\"Maybe I'll give it a try too.\"\r\n"},
        {"客Ａ\r\n「今日も一番乗りで来たぜ」\r\n",
         "Customer A\r\n\"Made it first in line again today.\"\r\n"},
        {"客Ｂ\r\n「メニューを見るだけでもワクワクするな」\r\n",
         "Customer B\r\n\"Just looking at the menu gets me excited.\"\r\n"},
        {"遥\r\n「準備はばっちりみたいね」\r\n",
         "Haruka\r\n\"Looks like preparations are all set.\"\r\n"},
        {"美弥香\r\n「開店時間になりましたわ」\r\n",
         "Miyaka\r\n\"It is opening time now.\"\r\n"},
        {"客Ｃ\r\n「よし、中に入ろう！」\r\n",
         "Customer C\r\n\"Alright, let's head inside!\"\r\n"},
    };

    const std::vector<std::pair<std::string, std::string>> precached_after = {
        {"夢美\r\n「それでは、ご注文が決まりましたらお呼び下さい」\r\n",
         "Yumemi\r\n\"Please call for us once you have decided on your order.\"\r\n"},
        {"客Ｄ\r\n「まずはホットコーヒーをひとつ頼むよ」\r\n",
         "Customer D\r\n\"I'll start with one hot coffee, please.\"\r\n"},
    };

    bj::array strings;
    translate::Cache seed_cache;
    for (const auto& [jp, en] : precached_before) {
        strings.push_back(MakeExtractedStringEntry("dialogue", jp));
        seed_cache.set(jp, en);
    }

    strings.push_back(MakeExtractedStringEntry("narrative", kNarrTownJp));
    strings.push_back(MakeExtractedStringEntry("narrative", kNarrCafeAnzuJp));
    strings.push_back(MakeExtractedStringEntry("dialogue", kDlgYumemiJp));
    strings.push_back(MakeExtractedStringEntry("dialogue", kDlgAgehaJp));
    strings.push_back(MakeExtractedStringEntry("dialogue", kDlgAnzuJp));

    for (const auto& [jp, en] : precached_after) {
        strings.push_back(MakeExtractedStringEntry("dialogue", jp));
        seed_cache.set(jp, en);
    }

    bj::object tokuten;
    tokuten["strings"] = std::move(strings);
    bj::object extracted_root;
    extracted_root["tokuten.spt"] = std::move(tokuten);

    const fs::path extracted_path = staged_script_out / "extracted_text.json";
    const fs::path cache_path = staged_script_out / "translation_cache_gemini.json";
    const fs::path translated_path = staged_script_out / "translated_text.json";
    const fs::path last_telemetry_path =
        staged_script_out / "last_gemini_translate" / "last_translation.json";
    const fs::path tsv_path = staged_game / "translation_table.tsv";

    write_file_text(extracted_path.u8string(),
                    json_pretty(bj::value(std::move(extracted_root))));
    translate::save_cache(seed_cache, cache_path.u8string());

    const std::vector<std::string> args = {
        "--dir", staged_project.u8string(),
        "--game-dir", staged_game.u8string(),
        "--batch", std::to_string(translate::kDefaultBatchSize),
        "--max-batches", std::to_string(kLiveMaxBatches),
    };

    // Pass 1: Live Gemini Carbon translation of the 5 uncached AnzU_CE .spt lines.
    const int rc_pass1 = apps::run_child_process(build_tsv_exe.u8string(), args);
    ASSERT_EQ(rc_pass1, apps::kExitOk)
        << "Pass 1 02_build_tsv.exe exited with non-zero code " << rc_pass1;

    ASSERT_TRUE(fs::is_regular_file(last_telemetry_path))
        << "Missing batch telemetry file: " << last_telemetry_path.u8string();
    const bj::object telemetry = json_parse_file(last_telemetry_path.u8string()).as_object();
    EXPECT_EQ(std::string_view(telemetry.at("model").as_string()),
              vn::llm::kDefaultModel);
    EXPECT_EQ(std::string_view(telemetry.at("thinking_level").as_string()),
              vn::llm::kDefaultThinkingLevel);
    EXPECT_EQ(telemetry.at("line_count").as_int64(), kExpectedLiveLineCount);

    const std::int64_t prompt_tokens = telemetry.at("prompt_tokens").as_int64();
    const std::int64_t candidates_tokens = telemetry.at("candidates_tokens").as_int64();
    const std::int64_t thought_tokens = telemetry.at("thought_tokens").as_int64();
    EXPECT_GT(prompt_tokens, 0);
    EXPECT_GT(candidates_tokens, 0);
    EXPECT_GT(thought_tokens, 0)
        << "gemini-4-carbon with thinkingLevel=MEDIUM must produce positive thought_tokens";

    const translate::Cache pass1_cache = translate::load_cache(cache_path.u8string());
    const std::array<const char*, static_cast<std::size_t>(kExpectedLiveLineCount)>
        kLiveJpKeys = {{
            kNarrTownJp,
            kNarrCafeAnzuJp,
            kDlgYumemiJp,
            kDlgAgehaJp,
            kDlgAnzuJp,
        }};
    for (const char* jp_key : kLiveJpKeys) {
        SCOPED_TRACE(jp_key);
        const std::string* en = pass1_cache.get(jp_key);
        ASSERT_NE(en, nullptr) << "Missing cached translation";
        EXPECT_FALSE(en->empty());
        EXPECT_TRUE(EndsWithCrlf(*en)) << "Expected trailing CRLF preserved: " << *en;
        EXPECT_TRUE(utf8_to_cp932_strict(*en).has_value())
            << "Translated string is not 100% strict CP932-clean: " << *en;
    }

    const std::string& town_en = *pass1_cache.get(kNarrTownJp);
    const std::string& cafe_en = *pass1_cache.get(kNarrCafeAnzuJp);
    const std::string& yumemi_en = *pass1_cache.get(kDlgYumemiJp);
    const std::string& ageha_en = *pass1_cache.get(kDlgAgehaJp);
    const std::string& anzu_en = *pass1_cache.get(kDlgAnzuJp);

    EXPECT_TRUE(AsciiContainsIgnoreCase(town_en, "Masuzugasaki")) << town_en;
    EXPECT_TRUE(AsciiContainsIgnoreCase(cafe_en, "AnzU")) << cafe_en;

    EXPECT_EQ(yumemi_en.rfind("Yumemi\r\n", 0), 0u) << yumemi_en;
    EXPECT_TRUE(AsciiContainsIgnoreCase(yumemi_en, "AnzU")) << yumemi_en;

    EXPECT_EQ(ageha_en.rfind("Ageha\r\n", 0), 0u) << ageha_en;

    EXPECT_EQ(anzu_en.rfind("Anzu\r\n", 0), 0u) << anzu_en;
    EXPECT_TRUE(AsciiContainsIgnoreCase(anzu_en, "Yumemi")) << anzu_en;
    EXPECT_TRUE(AsciiContainsIgnoreCase(anzu_en, "Ageha")) << anzu_en;

    ASSERT_TRUE(fs::is_regular_file(translated_path));
    ASSERT_TRUE(fs::is_regular_file(tsv_path));
    const Bytes raw_tsv_pass1 = read_file(tsv_path.u8string());
    const auto tsv_utf8_pass1 = cp932_to_utf8_strict(raw_tsv_pass1);
    ASSERT_TRUE(tsv_utf8_pass1.has_value())
        << "Generated translation_table.tsv must be strict CP932";
    EXPECT_NE(tsv_utf8_pass1->find("Yumemi: \""), std::string::npos);
    EXPECT_NE(tsv_utf8_pass1->find("Ageha: \""), std::string::npos);
    EXPECT_NE(tsv_utf8_pass1->find("Anzu: \""), std::string::npos);

    // Feed generated CP932 TSV through real proxy_dll parser and confirm every
    // live-translated line resolves at runtime without Japanese leaks.
    translator_logic::TranslationMap runtime_map;
    ASSERT_GT(translator_logic::ParseTsvBuffer(
                  reinterpret_cast<const char*>(raw_tsv_pass1.data()),
                  raw_tsv_pass1.size(),
                  runtime_map),
              0);
    for (const char* jp_key : kLiveJpKeys) {
        SCOPED_TRACE(jp_key);
        const auto cp932_key = utf8_to_cp932_strict(jp_key);
        ASSERT_TRUE(cp932_key.has_value());
        const std::string key(reinterpret_cast<const char*>(cp932_key->data()), cp932_key->size());
        std::string resolved_en;
        ASSERT_TRUE(translator_logic::LookupTranslation(runtime_map, key.c_str(), false, resolved_en))
            << "Proxy lookup missed live-translated line";
        EXPECT_FALSE(translate::has_real_japanese(cp932_to_utf8_replace(
            reinterpret_cast<const std::uint8_t*>(resolved_en.data()), resolved_en.size())));
    }

    // Pass 2: Immediate resume must execute 0 API calls (verified by deleting
    // last_translation.json prior to Pass 2 and confirming it is NOT recreated).
    std::error_code rm_ec;
    ASSERT_TRUE(fs::remove(last_telemetry_path, rm_ec)) << rm_ec.message();
    ASSERT_FALSE(fs::exists(last_telemetry_path));

    const int rc_pass2 = apps::run_child_process(build_tsv_exe.u8string(), args);
    ASSERT_EQ(rc_pass2, apps::kExitOk)
        << "Pass 2 resume 02_build_tsv.exe exited with non-zero code " << rc_pass2;
    EXPECT_FALSE(fs::exists(last_telemetry_path))
        << "2nd-pass resume unexpectedly invoked the Gemini API!";

    const translate::Cache pass2_cache = translate::load_cache(cache_path.u8string());
    EXPECT_EQ(pass2_cache.items(), pass1_cache.items());
    EXPECT_TRUE(cp932_to_utf8_strict(read_file(tsv_path.u8string())).has_value());

    std::cout << "[LIVE ANZU_CE CARBON] thought_tokens=" << thought_tokens
              << " prompt_tokens=" << prompt_tokens
              << " candidates_tokens=" << candidates_tokens
              << " | anzu_plate=\"" << anzu_en << "\"" << std::endl;
}
