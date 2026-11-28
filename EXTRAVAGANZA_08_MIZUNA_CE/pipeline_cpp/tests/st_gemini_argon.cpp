// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Tier-2 game-specific live gemini-4-carbon system test (LABELS "SYSTEM"):
// runs the compiled 02_build_tsv.exe binary on authentic EXTRA_VA_MIZUNA_CE
// (.spt) lines and glossary speakers inside a self-cleaning temp directory,
// verifying MEDIUM reasoning activation (thought_tokens > 0), Speaker\r\n
// nameplate preservation (including 瑞菜 -> Mizuna canonicalization), valid
// CP932 translation_table.tsv output, and 0-API-call 2nd-pass resume.
#include "apps/paths.h"
#include "cache/cache_core.h"
#include "cache/gemini_client.h"
#include "common/util.h"
#include "vn/llm/llm_types.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace bj = boost::json;
namespace fs = std::filesystem;

constexpr int kMaxWalkLevels = 8;
constexpr std::size_t kBufferGrowthFactor = 2;
constexpr std::int64_t kExpectedSnippetLineCount = 5;
constexpr std::size_t kCrlfLen = 2;

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view prefix) {
        static std::atomic<std::uint64_t> counter{0};
        const std::uint64_t id = counter.fetch_add(1u, std::memory_order_relaxed);
        path_ = fs::temp_directory_path() /
                fs::u8path(std::string(prefix) + "_" +
                           std::to_string(GetCurrentProcessId()) + "_" +
                           std::to_string(id));
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

fs::path CurrentTestExecutableDir() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n =
            GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) {
            return {};
        }
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * kBufferGrowthFactor);
    }
    return fs::path(buf).parent_path();
}

fs::path FindGameCheckoutRootFrom(fs::path start) {
    std::error_code ec;
    for (int i = 0; i < kMaxWalkLevels && !start.empty(); ++i) {
        if (fs::is_regular_file(start / "build.py", ec) &&
            fs::is_directory(start / "pipeline_cpp", ec)) {
            return start;
        }
        const fs::path nested = start / "EXTRAVAGANZA_08_MIZUNA_CE";
        if (fs::is_regular_file(nested / "build.py", ec) &&
            fs::is_directory(nested / "pipeline_cpp", ec)) {
            return nested;
        }
        if (!start.has_parent_path() || start.parent_path() == start) {
            break;
        }
        start = start.parent_path();
    }
    return {};
}

fs::path FindBuildTsvExecutable(const fs::path& test_exe_dir,
                                const fs::path& checkout_root) {
    std::error_code ec;
    std::vector<fs::path> candidates;
    if (!checkout_root.empty()) {
        candidates.push_back(checkout_root / "build" / "install" / "bin" /
                             "02_build_tsv.exe");
        candidates.push_back(checkout_root / "build" / "pipeline" / "Release" /
                             "02_build_tsv.exe");
    }
    if (!test_exe_dir.empty()) {
        candidates.push_back(test_exe_dir / "02_build_tsv.exe");
        candidates.push_back(test_exe_dir.parent_path().parent_path() / "Release" /
                             "02_build_tsv.exe");
    }
    for (const auto& candidate : candidates) {
        if (fs::is_regular_file(candidate, ec)) {
            return candidate;
        }
    }
    return {};
}

bool ContainsAsciiCaseInsensitive(std::string_view haystack, std::string_view needle) {
    const auto it = std::search(
        haystack.begin(), haystack.end(), needle.begin(), needle.end(),
        [](unsigned char a, unsigned char b) {
            return std::tolower(a) == std::tolower(b);
        });
    return it != haystack.end();
}

bool EndsWithCrlf(std::string_view s) {
    return s.size() >= kCrlfLen &&
           s.substr(s.size() - kCrlfLen) == "\r\n";
}

bj::object MakeEntry(std::string_view type, std::string_view text) {
    bj::object entry;
    entry["type"] = type;
    entry["has_jp"] = true;
    entry["text"] = text;
    return entry;
}

TEST(GeminiArgonLiveSystem, MizunaCeLiveBatchPreservesSpeakerPlatesAndResumesWithZeroApiCalls) {
    const fs::path exe_dir = CurrentTestExecutableDir();
    const fs::path checkout_root =
        !exe_dir.empty() ? FindGameCheckoutRootFrom(exe_dir)
                         : FindGameCheckoutRootFrom(fs::current_path());

    const ScopedTempDir sandbox("mizuna_st_gemini_carbon");
    const std::string key_probe_dir =
        !checkout_root.empty() ? checkout_root.u8string() : sandbox.path().u8string();
    exm::gemini::load_api_key(key_probe_dir);

    const char* api_key = std::getenv("GEMINI_API_KEY");
    const bool has_key = api_key != nullptr && *api_key != '\0';
    if (!has_key) {
        const char* gha = std::getenv("GITHUB_ACTIONS");
        if (gha != nullptr && std::string_view(gha) == "true") {
            GTEST_SKIP() << "GEMINI_API_KEY is absent on GitHub Actions CI; "
                            "skipping live gemini-4-carbon system test.";
        }
        FAIL() << "GEMINI_API_KEY is missing locally (checked environment, "
                  ".claude/settings.json, ~/.claude_vn/settings.json, "
                  "~/.gemini/.env, and HKCU\\Environment).";
    }

    const fs::path build_tsv_exe = FindBuildTsvExecutable(exe_dir, checkout_root);
    ASSERT_FALSE(build_tsv_exe.empty())
        << "Compiled 02_build_tsv.exe binary not found near test executable.";

    const fs::path temp_project = sandbox.path() / "project";
    const fs::path temp_script_out = temp_project / "script_output";
    const fs::path temp_game = sandbox.path() / "game";
    fs::create_directories(temp_script_out);
    fs::create_directories(temp_game);

    const std::vector<std::pair<std::string, std::string>> precached_prefix = {
        {"水菜\r\n「……………………」\r\n", "Mizuna\r\n\"................\"\r\n"},
        {"史郎\r\n「これからが楽しみだよ」\r\n",
         "Shirou\r\n\"I'm looking forward to what comes next.\"\r\n"},
        {"水菜\r\n「……あなたみたいな変態と一緒にしないで」\r\n",
         "Mizuna\r\n\"...Don't lump me in with a pervert like you.\"\r\n"},
        {"史郎\r\n「少しは反省したかなぁ～？」\r\n",
         "Shirou\r\n\"Have you learned your lesson yet~?\"\r\n"},
        {"水菜\r\n「寝言は……寝てから言いなさい」\r\n",
         "Mizuna\r\n\"Save your sleep talking... for when you're asleep.\"\r\n"},
        {"史郎\r\n「あれ、今ごろ気づいたの？」\r\n",
         "Shirou\r\n\"Huh, you just realized that now?\"\r\n"},
        {"男Ａ\r\n「おい、早くしろよ。あとがつかえてんだから」\r\n",
         "Man A\r\n\"Hey, hurry it up. There's a line waiting behind you.\"\r\n"},
        {"？？？\r\n「……無駄な抵抗はやめろ」\r\n",
         "???\r\n\"...Stop your pointless resistance.\"\r\n"},
        {"瑞菜\r\n「まだ終わりじゃないわ……」\r\n",
         "Mizuna\r\n\"It isn't over yet...\"\r\n"},
        {"史郎\r\n「ボクはこれから学校に行ってくるからさ」\r\n",
         "Shirou\r\n\"I've gotta head off to school now.\"\r\n"},
    };

    constexpr std::string_view kShirouWelcomeJp =
        "史郎\r\n「うひひっ、ようこそ水菜ちゃん」\r\n";
    constexpr std::string_view kGenbuNarrationJp =
        "玄武の巫女として歪と戦っていた水菜は、\r\n"
        "北畠史郎と名乗る人物に捕まってしまっていた。\r\n";
    constexpr std::string_view kDimRoomNarrationJp =
        "まともな説明などされないまま、\r\n"
        "薄暗い部屋で拘束されてしまっていた。\r\n";
    constexpr std::string_view kMizunaQuestionJp =
        "水菜\r\n「……私をどうするつもり？」\r\n";
    constexpr std::string_view kMizunaAltKanjiJp =
        "瑞菜\r\n「たとえ蟲使いが相手でも、心までは屈しないわ」\r\n";

    const std::vector<std::pair<std::string_view, std::string_view>> uncached_mizuna_lines = {
        {"dialogue", kShirouWelcomeJp},
        {"narrative", kGenbuNarrationJp},
        {"narrative", kDimRoomNarrationJp},
        {"dialogue", kMizunaQuestionJp},
        {"dialogue", kMizunaAltKanjiJp},
    };

    const std::vector<std::tuple<std::string_view, std::string, std::string>> precached_lookahead = {
        {"dialogue",
         "史郎\r\n「うーん、そうだなぁ。まずは………………」\r\n",
         "Shirou\r\n\"Hmm, let's see. First of all............\"\r\n"},
        {"narrative",
         "反省したと答えたところで、自分の扱いは変わらない。\r\n",
         "Even if she told him she had learned her lesson, the way he treated her wouldn't change.\r\n"},
    };

    bj::array strings;
    bj::object cache_obj;
    for (const auto& [jp, en] : precached_prefix) {
        strings.push_back(MakeEntry("dialogue", jp));
        cache_obj[jp] = en;
    }
    for (const auto& [type, jp] : uncached_mizuna_lines) {
        strings.push_back(MakeEntry(type, jp));
    }
    for (const auto& [type, jp, en] : precached_lookahead) {
        strings.push_back(MakeEntry(type, jp));
        cache_obj[jp] = en;
    }

    bj::object script_obj;
    script_obj["strings"] = std::move(strings);
    bj::object extracted_root;
    extracted_root["tantai3.spt"] = std::move(script_obj);

    const fs::path extracted_path = temp_script_out / "extracted_text.json";
    const fs::path cache_path = temp_script_out / "translation_cache_gemini.json";
    const fs::path translated_path = temp_script_out / "translated_text.json";
    const fs::path last_batch_path =
        temp_script_out / "last_gemini_translate" / "last_translation.json";
    const fs::path out_tsv_path = temp_game / "translation_table.tsv";

    exm::write_file_text(extracted_path.u8string(), exm::json_pretty(bj::value(extracted_root)));
    exm::write_file_text(cache_path.u8string(), exm::json_pretty(bj::value(cache_obj)));

    const std::vector<std::string> pass_args = {
        "--dir", temp_project.u8string(),
        "--game-dir", temp_game.u8string(),
        "--batch", std::to_string(exm::translate::kDefaultBatchSize),
        "--max-batches", "1",
    };

    const auto pass1_start = std::chrono::steady_clock::now();
    const int rc_pass1 = exm::apps::run_process(build_tsv_exe.u8string(), pass_args);
    const double pass1_sec =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - pass1_start).count();
    ASSERT_EQ(rc_pass1, exm::apps::kExitOk) << "Pass 1 of 02_build_tsv.exe failed";
    EXPECT_LT(pass1_sec, static_cast<double>(vn::llm::kDefaultReadTimeoutSeconds));

    ASSERT_TRUE(fs::exists(last_batch_path))
        << "Expected batch telemetry file at " << last_batch_path.u8string();
    const bj::value last_batch_val = exm::json_parse_file(last_batch_path.u8string());
    ASSERT_TRUE(last_batch_val.is_object());
    const bj::object& last_batch = last_batch_val.get_object();

    EXPECT_EQ(last_batch.at("line_count").as_int64(), kExpectedSnippetLineCount);
    const std::int64_t thought_tokens = last_batch.at("thought_tokens").as_int64();
    const std::int64_t prompt_tokens = last_batch.at("prompt_tokens").as_int64();
    const std::int64_t candidates_tokens = last_batch.at("candidates_tokens").as_int64();

    EXPECT_GT(thought_tokens, 0)
        << "gemini-4-carbon with thinkingLevel=MEDIUM must produce thought_tokens > 0";
    EXPECT_GT(prompt_tokens, 0);
    EXPECT_GT(candidates_tokens, 0);

    const exm::translate::Cache updated_cache =
        exm::translate::load_cache(cache_path.u8string());

    const std::string* shirou_welcome_en = updated_cache.get(std::string(kShirouWelcomeJp));
    ASSERT_NE(shirou_welcome_en, nullptr);
    EXPECT_EQ(shirou_welcome_en->rfind("Shirou\r\n", 0), 0u)
        << "Expected Shirou\\r\\n nameplate prefix, got: " << *shirou_welcome_en;
    EXPECT_TRUE(EndsWithCrlf(*shirou_welcome_en));
    EXPECT_TRUE(ContainsAsciiCaseInsensitive(*shirou_welcome_en, "Mizuna"));
    EXPECT_EQ(exm::jp_char_count(*shirou_welcome_en), 0);

    const std::string* genbu_en = updated_cache.get(std::string(kGenbuNarrationJp));
    ASSERT_NE(genbu_en, nullptr);
    EXPECT_TRUE(EndsWithCrlf(*genbu_en));
    EXPECT_TRUE(ContainsAsciiCaseInsensitive(*genbu_en, "Mizuna"));
    EXPECT_TRUE(ContainsAsciiCaseInsensitive(*genbu_en, "Shirou"));
    EXPECT_EQ(exm::jp_char_count(*genbu_en), 0);

    const std::string* dim_room_en = updated_cache.get(std::string(kDimRoomNarrationJp));
    ASSERT_NE(dim_room_en, nullptr);
    EXPECT_TRUE(EndsWithCrlf(*dim_room_en));
    EXPECT_EQ(exm::jp_char_count(*dim_room_en), 0);

    const std::string* mizuna_q_en = updated_cache.get(std::string(kMizunaQuestionJp));
    ASSERT_NE(mizuna_q_en, nullptr);
    EXPECT_EQ(mizuna_q_en->rfind("Mizuna\r\n", 0), 0u)
        << "Expected Mizuna\\r\\n nameplate prefix, got: " << *mizuna_q_en;
    EXPECT_TRUE(EndsWithCrlf(*mizuna_q_en));
    EXPECT_EQ(exm::jp_char_count(*mizuna_q_en), 0);

    const std::string* mizuna_alt_en = updated_cache.get(std::string(kMizunaAltKanjiJp));
    ASSERT_NE(mizuna_alt_en, nullptr);
    EXPECT_EQ(mizuna_alt_en->rfind("Mizuna\r\n", 0), 0u)
        << "Expected 瑞菜 -> Mizuna\\r\\n nameplate canonicalization, got: "
        << *mizuna_alt_en;
    EXPECT_TRUE(EndsWithCrlf(*mizuna_alt_en));
    EXPECT_EQ(exm::jp_char_count(*mizuna_alt_en), 0);

    ASSERT_TRUE(fs::exists(translated_path));
    ASSERT_TRUE(fs::exists(out_tsv_path));
    const exm::Bytes tsv_bytes_pass1 = exm::read_file(out_tsv_path.u8string());
    const auto tsv_utf8 = exm::cp932_to_utf8_strict(tsv_bytes_pass1);
    ASSERT_TRUE(tsv_utf8.has_value()) << "Generated translation_table.tsv is not valid CP932";
    EXPECT_NE(tsv_utf8->find("史郎\\r\\n「うひひっ、ようこそ水菜ちゃん」\\r\\n\tShirou: "),
              std::string::npos);
    EXPECT_NE(tsv_utf8->find("水菜\\r\\n「……私をどうするつもり？」\\r\\n\tMizuna: "),
              std::string::npos);
    EXPECT_NE(tsv_utf8->find(
                  "瑞菜\\r\\n「たとえ蟲使いが相手でも、心までは屈しないわ」\\r\\n\tMizuna: "),
              std::string::npos);

    std::size_t tsv_pos = 0;
    std::size_t tsv_rows = 0;
    while (tsv_pos < tsv_utf8->size()) {
        const std::size_t nl = tsv_utf8->find('\n', tsv_pos);
        const std::string_view row =
            nl == std::string::npos
                ? std::string_view(*tsv_utf8).substr(tsv_pos)
                : std::string_view(*tsv_utf8).substr(tsv_pos, nl - tsv_pos);
        tsv_pos = nl == std::string::npos ? tsv_utf8->size() : nl + 1;
        if (row.empty()) {
            continue;
        }
        ++tsv_rows;
        const std::size_t tab = row.find('\t');
        ASSERT_NE(tab, std::string_view::npos) << "Malformed TSV row: " << row;
        EXPECT_EQ(row.find('\t', tab + 1), std::string_view::npos)
            << "Multiple tabs in TSV row: " << row;
        const std::string en_col(row.substr(tab + 1));
        EXPECT_EQ(exm::jp_char_count(en_col), 0)
            << "Japanese characters leaked into TSV English column: " << en_col;
    }
    EXPECT_GT(tsv_rows, 0u);

    std::cout << "[ST_GEMINI_CARBON] model=" << vn::llm::kDefaultModel
              << " thinking_level=" << vn::llm::kDefaultThinkingLevel
              << " thought_tokens=" << thought_tokens
              << " prompt_tokens=" << prompt_tokens
              << " candidates_tokens=" << candidates_tokens
              << " pass1_sec=" << pass1_sec
              << " mizuna_q_en=\"" << *mizuna_q_en << "\"\n";

    // Immediate 2nd-pass resume: delete last_translation.json and verify 0 API calls.
    const exm::Bytes cache_bytes_pass1 = exm::read_file(cache_path.u8string());
    ASSERT_TRUE(fs::remove(last_batch_path));
    ASSERT_FALSE(fs::exists(last_batch_path));

    const int rc_pass2 = exm::apps::run_process(build_tsv_exe.u8string(), pass_args);
    ASSERT_EQ(rc_pass2, exm::apps::kExitOk) << "Pass 2 resume of 02_build_tsv.exe failed";
    EXPECT_FALSE(fs::exists(last_batch_path))
        << "2nd-pass resume unexpectedly made an API call and recreated last_translation.json";
    EXPECT_EQ(exm::read_file(cache_path.u8string()), cache_bytes_pass1);
    EXPECT_EQ(exm::read_file(out_tsv_path.u8string()), tsv_bytes_pass1);
}

}  // namespace
