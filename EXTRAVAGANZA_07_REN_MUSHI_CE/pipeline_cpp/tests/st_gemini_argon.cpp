// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE. Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved. See NOTICE.

// Game-specific Tier-2 live SYSTEM test executing built 02_build_tsv.exe
// against live gemini-4-carbon on authentic EXTRAVAGANZA_07_REN_MUSHI_CE
// (BLACKCyc .spt) script lines, speaker glossary plates, CP932 TSV output,
// and 0-API-call 2nd-pass resume idempotency inside a hermetic ScopedTempDir.
#include "apps/paths.h"
#include "cache/cache_core.h"
#include "cache/gemini_client.h"
#include "cache/glossary.h"
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
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>

namespace {

namespace bj = boost::json;
namespace fs = std::filesystem;
namespace tr = exm::translate;

constexpr std::size_t kCachedSpeakerGateDialogueCount = 30;
constexpr std::size_t kLiveMushiSliceCount = 6;
constexpr int kMaxBatchesArg = 1;
constexpr int kJsonIndent = 2;
constexpr std::size_t kCrlfLen = 2;
constexpr std::size_t kSingleTabCount = 1;

constexpr const char* kYumemiFarewellJp =
    "夢美\r\n「行ってきます。いいコにしてるのよ」\r\n";
constexpr const char* kMushiTentaclesJp =
    "そこには一匹の蟲がちょこんといて、ピンク色の触手\r\nをゆらゆらと揺らしていた。\r\n";
constexpr const char* kRenDemonBugJp =
    "レン\r\n「これが獣魔蟲……。\r\n　確かに普通の蟲ではありませんね」\r\n";
constexpr const char* kEmceeSongJp =
    "司会者\r\n「それでは歌っていただきましょう。\r\n　曲は――――――」\r\n";
constexpr const char* kSausagePackJp =
    "レンが取り出したのは、１２本がパックになった\r\nソーセージ。\r\n";
constexpr const char* kRenThanksJp =
    "レン\r\n「……ありがとうございます」\r\n";

struct ScriptSliceRow {
    const char* type;
    const char* jp;
    const char* expected_speaker_prefix;
    const char* expected_tsv_speaker_prefix;
};

constexpr std::array<ScriptSliceRow, kLiveMushiSliceCount> kLiveMushiLines = {{
    {"dialogue", kYumemiFarewellJp, "Yumemi\r\n", "Yumemi: "},
    {"narrative", kMushiTentaclesJp, nullptr, nullptr},
    {"dialogue", kRenDemonBugJp, "Ren\r\n", "Ren: "},
    {"dialogue", kEmceeSongJp, "Emcee\r\n", "Emcee: "},
    {"narrative", kSausagePackJp, nullptr, nullptr},
    {"dialogue", kRenThanksJp, "Ren\r\n", "Ren: "},
}};

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view prefix) {
        static std::atomic<std::uint64_t> seq{0};
        const std::uint64_t id = seq.fetch_add(1u, std::memory_order_relaxed);
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

class ScopedEnvOverride {
public:
    ScopedEnvOverride(const char* name, const char* value) : name_(name) {
        if (const char* cur = std::getenv(name)) {
            had_original_ = true;
            original_ = cur;
        }
        _putenv_s(name_.c_str(), value);
    }

    ~ScopedEnvOverride() {
        _putenv_s(name_.c_str(), had_original_ ? original_.c_str() : "");
    }

    ScopedEnvOverride(const ScopedEnvOverride&) = delete;
    ScopedEnvOverride& operator=(const ScopedEnvOverride&) = delete;

private:
    std::string name_;
    std::string original_;
    bool had_original_ = false;
};

bool HasNonEmptyEnv(const char* name) {
    const char* val = std::getenv(name);
    return val != nullptr && *val != '\0';
}

bool IsGitHubActions() {
    const char* gha = std::getenv("GITHUB_ACTIONS");
    return gha != nullptr && std::string_view(gha) == "true";
}

bool EndsWithCrlf(std::string_view s) noexcept {
    return s.size() >= kCrlfLen &&
           s.substr(s.size() - kCrlfLen) == "\r\n";
}

fs::path LocateBuiltBuildTsvBinary() {
    const fs::path bin_dir = fs::u8path(exm::apps::exe_dir());
    const std::string project_dir = exm::apps::find_project_dir_from(bin_dir);
    std::error_code ec;
    if (!project_dir.empty()) {
        const fs::path root = fs::u8path(project_dir);
        for (const fs::path& candidate : {
                 root / "build" / "pipeline" / "Release" / "02_build_tsv.exe",
                 root / "build" / "install" / "bin" / "02_build_tsv.exe",
                 bin_dir / "02_build_tsv.exe",
             }) {
            if (fs::exists(candidate, ec)) {
                return candidate;
            }
        }
    }
    const fs::path sibling_release =
        bin_dir.parent_path().parent_path() / "Release" / "02_build_tsv.exe";
    if (fs::exists(sibling_release, ec)) {
        return sibling_release;
    }
    return bin_dir / "02_build_tsv.exe";
}

bj::object MakeExtractedStringEntry(std::string_view type, std::string_view text) {
    bj::object entry;
    entry["type"] = std::string(type);
    entry["text"] = std::string(text);
    entry["has_jp"] = true;
    return entry;
}

void StageHermeticRenMushiFixture(const fs::path& project_dir) {
    const fs::path out_dir = project_dir / "script_output";
    fs::create_directories(out_dir);

    bj::array cached_dialogue;
    tr::Cache seeded_cache;
    const auto& speakers = tr::name_translations_ordered();
    for (std::size_t i = 0; i < kCachedSpeakerGateDialogueCount; ++i) {
        const auto& [jp_speaker, en_speaker] = speakers[i % speakers.size()];
        const std::string jp_line =
            jp_speaker + "\r\n「既訳ゲート確認行その" + std::to_string(i + 1) + "です。」\r\n";
        const std::string en_line =
            en_speaker + "\r\n\"Cached speaker gate verification line " +
            std::to_string(i + 1) + ".\"\r\n";
        cached_dialogue.push_back(MakeExtractedStringEntry("dialogue", jp_line));
        seeded_cache.set(jp_line, en_line);
    }

    bj::array mushi_strings;
    for (const auto& row : kLiveMushiLines) {
        mushi_strings.push_back(MakeExtractedStringEntry(row.type, row.jp));
    }

    bj::object extracted_root;
    extracted_root["mushi.spt"] = bj::object{{"strings", std::move(mushi_strings)}};
    extracted_root["backscript.spt"] = bj::object{{"strings", std::move(cached_dialogue)}};

    exm::write_file_text(
        (out_dir / "extracted_text.json").u8string(),
        exm::json_pretty(bj::value(std::move(extracted_root)), kJsonIndent));
    tr::save_cache(seeded_cache, (out_dir / "translation_cache_gemini.json").u8string());
}

}  // namespace

TEST(GeminiArgonSystemTest, LiveRenMushiE2EPreservesSpeakerPlatesCp932AndZeroCallResume) {
    const std::string project_hint =
        exm::apps::find_project_dir_from(fs::u8path(exm::apps::exe_dir()));
    exm::gemini::load_api_key(project_hint);

    if (!HasNonEmptyEnv("GEMINI_API_KEY")) {
        if (IsGitHubActions()) {
            GTEST_SKIP()
                << "GITHUB_ACTIONS=true and GEMINI_API_KEY is absent; skipping live gemini-4-carbon SYSTEM test.";
        }
        FAIL()
            << "GEMINI_API_KEY is missing locally (checked environment, ~/.gemini/.env, HKCU\\Environment, and .claude/settings.json).";
    }

    const fs::path tool_exe = LocateBuiltBuildTsvBinary();
    ASSERT_TRUE(fs::exists(tool_exe))
        << "02_build_tsv.exe missing at " << tool_exe.u8string();

    const ScopedTempDir sandbox("ren_mushi_st_live_e2e");
    const fs::path project_dir = sandbox.path() / "project";
    const fs::path game_dir = sandbox.path() / "game";
    const fs::path out_dir = project_dir / "script_output";
    const fs::path cache_path = out_dir / "translation_cache_gemini.json";
    const fs::path output_path = out_dir / "translated_text.json";
    const fs::path tsv_path = game_dir / "translation_table.tsv";
    const fs::path telemetry_path =
        out_dir / "last_gemini_translate" / "last_translation.json";
    fs::create_directories(game_dir);

    StageHermeticRenMushiFixture(project_dir);

    const std::vector<std::string> args = {
        "--dir",
        project_dir.u8string(),
        "--game-dir",
        game_dir.u8string(),
        "--batch",
        std::to_string(tr::kDefaultBatchSize),
        "--max-batches",
        std::to_string(kMaxBatchesArg),
        "--out",
        tsv_path.u8string(),
    };

    const auto pass1_start = std::chrono::steady_clock::now();
    const int rc1 = exm::apps::run_process(tool_exe.u8string(), args);
    const double pass1_sec =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - pass1_start)
            .count();
    ASSERT_EQ(rc1, exm::apps::kExitOk) << "Pass 1 02_build_tsv.exe failed";
    EXPECT_LT(pass1_sec, static_cast<double>(vn::llm::kDefaultReadTimeoutSeconds));

    ASSERT_TRUE(fs::exists(telemetry_path))
        << "Missing batch telemetry file after Pass 1: " << telemetry_path.u8string();
    const bj::object last_doc = exm::json_parse_file(telemetry_path.u8string()).as_object();
    EXPECT_EQ(std::string(last_doc.at("model").as_string()),
              std::string(vn::llm::kDefaultModel));
    EXPECT_EQ(std::string(last_doc.at("thinking_level").as_string()),
              std::string(vn::llm::kDefaultThinkingLevel));
    EXPECT_EQ(last_doc.at("line_count").as_int64(),
              static_cast<std::int64_t>(kLiveMushiSliceCount));

    const std::int64_t prompt_tokens = last_doc.at("prompt_tokens").as_int64();
    const std::int64_t candidate_tokens = last_doc.at("candidate_tokens").as_int64();
    const std::int64_t thought_tokens = last_doc.at("thought_tokens").as_int64();
    EXPECT_GT(prompt_tokens, 0);
    EXPECT_GT(candidate_tokens, 0);
    EXPECT_GT(thought_tokens, 0)
        << "gemini-4-carbon (thinkingLevel=MEDIUM) returned 0 thought tokens on live Ren_mushi_CE batch.";

    const tr::Cache pass1_cache = tr::load_cache(cache_path.u8string());
    std::cout << "[ST Ren_mushi_CE telemetry] model=" << vn::llm::kDefaultModel
              << " thinking_level=" << vn::llm::kDefaultThinkingLevel
              << " latency_sec=" << pass1_sec
              << " prompt_tokens=" << prompt_tokens
              << " candidate_tokens=" << candidate_tokens
              << " thought_tokens=" << thought_tokens << std::endl;

    for (const auto& row : kLiveMushiLines) {
        SCOPED_TRACE(row.jp);
        const std::string* en = pass1_cache.get(row.jp);
        ASSERT_NE(en, nullptr) << "Missing cached translation for: " << row.jp;
        EXPECT_FALSE(en->empty());
        EXPECT_FALSE(tr::is_failed_entry(row.jp, *en));
        EXPECT_FALSE(tr::has_real_japanese(*en)) << "Residual Japanese in: " << *en;
        EXPECT_TRUE(EndsWithCrlf(*en)) << "Missing trailing CRLF in: " << *en;
        if (row.expected_speaker_prefix != nullptr) {
            EXPECT_EQ(en->rfind(row.expected_speaker_prefix, 0), 0u)
                << "Missing Speaker\\r\\n plate on: " << *en;
        }
    }

    ASSERT_TRUE(fs::exists(output_path));
    ASSERT_TRUE(fs::exists(tsv_path));
    const exm::Bytes pass1_cache_bytes = exm::read_file(cache_path.u8string());
    const exm::Bytes pass1_tsv_bytes = exm::read_file(tsv_path.u8string());
    ASSERT_FALSE(pass1_tsv_bytes.empty());

    const auto tsv_utf8 = exm::cp932_to_utf8_strict(pass1_tsv_bytes);
    ASSERT_TRUE(tsv_utf8.has_value())
        << "translation_table.tsv contains non-CP932 bytes";

    std::size_t tsv_row_count = 0;
    std::size_t line_start = 0;
    while (line_start < tsv_utf8->size()) {
        const std::size_t nl = tsv_utf8->find('\n', line_start);
        const std::size_t line_end =
            nl == std::string::npos ? tsv_utf8->size() : nl;
        const std::string_view row_view(tsv_utf8->data() + line_start,
                                        line_end - line_start);
        if (!row_view.empty()) {
            ++tsv_row_count;
            EXPECT_EQ(std::count(row_view.begin(), row_view.end(), '\t'),
                      static_cast<std::ptrdiff_t>(kSingleTabCount))
                << row_view;
        }
        if (nl == std::string::npos) {
            break;
        }
        line_start = nl + 1;
    }
    EXPECT_EQ(tsv_row_count,
              kCachedSpeakerGateDialogueCount + kLiveMushiSliceCount);

    for (const auto& row : kLiveMushiLines) {
        if (row.expected_tsv_speaker_prefix == nullptr) {
            continue;
        }
        EXPECT_NE(tsv_utf8->find(row.expected_tsv_speaker_prefix), std::string::npos)
            << "Missing merged speaker prefix in TSV: " << row.expected_tsv_speaker_prefix;
    }

    fs::remove(telemetry_path);
    ASSERT_FALSE(fs::exists(telemetry_path));

    {
        const ScopedEnvOverride poison_key(
            "GEMINI_API_KEY", "RESUME_PASS_MUST_MAKE_ZERO_API_CALLS");
        const int rc2 = exm::apps::run_process(tool_exe.u8string(), args);
        ASSERT_EQ(rc2, exm::apps::kExitOk)
            << "Immediate 2nd-pass resume failed or attempted a network API call";
    }

    EXPECT_FALSE(fs::exists(telemetry_path))
        << "2nd-pass resume unexpectedly executed an API batch";
    EXPECT_EQ(exm::read_file(cache_path.u8string()), pass1_cache_bytes);
    EXPECT_EQ(exm::read_file(tsv_path.u8string()), pass1_tsv_bytes);
}
