// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Black-box component tests for Adieu's translation subsystem (`translate_all` <->
// `gemini::Client` <-> `vn::llm_client` <-> `ZeroThoughtCircuitBreaker` <-> disk
// `Cache` <-> `build_tsv` <-> compiled `02_translate.exe` CLI).
// Uses 100% real internal collaborators inside `ScopedTempDir`, isolating only the
// outer HTTPS boundary via `vn::llm::IRawHttpTransport`.
#include "apps/paths.h"
#include "common/util.h"
#include "translate/gemini_client.h"
#include "translate/glossary.h"
#include "translate/translate_core.h"

#if defined(_WINDOWS_) || defined(_INC_WINDOWS) || defined(_WINSOCKAPI_) || \
    defined(OPENSSL_VERSION_NUMBER)
#error "Public translate/apps headers leaked Windows, Winsock, or OpenSSL headers"
#endif

#include "vn/llm/llm_client.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace adi::translate {
namespace {

namespace bj = boost::json;
namespace fs = std::filesystem;

constexpr int kHttpOk = 200;
constexpr int kHttpTooManyRequests = 429;
constexpr int kComponentBatchSize = 16;
constexpr int kNoBatchLimit = 0;
constexpr int kExitSuccess = 0;
constexpr std::int64_t kHealthyPromptTokens = 420;
constexpr std::int64_t kHealthyCandidateTokens = 180;
constexpr std::int64_t kHealthyThoughtTokens = 260;
constexpr std::int64_t kZeroThoughtTokens = 0;
constexpr std::size_t kBatchLineCount = 16u;
constexpr std::size_t kUtf8BomBytes = 3u;
constexpr std::int64_t kExpectedBackoffMs = 60000;

constexpr const char* kStartScene = "_start";
constexpr const char* kPrologueScene = "seen900";
constexpr const char* kMainScene = "seen020";
constexpr const char* kNamaeA = "＊Ａ";
constexpr const char* kNamaeB = "＊Ｂ";
constexpr const char* kHitomiJp = "人見";
constexpr const char* kKousukeJp = "広介";
constexpr const char* kMutsukiJp = "睦　月";
constexpr const char* kTonaeJp = "となえ";

class ScopedTempDir {
public:
    ScopedTempDir() {
        static std::atomic<unsigned int> seq{0u};
        dir_ = fs::temp_directory_path() /
               fs::u8path("adi_ct_translate_さよなら_" + std::to_string(++seq));
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_, ec);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const fs::path& path() const noexcept { return dir_; }

private:
    fs::path dir_;
};

struct QueuedHttpOutcome {
    int status_code = kHttpOk;
    std::int64_t thought_tokens = kHealthyThoughtTokens;
    std::string raw_error_body;
};

// Outer HTTPS boundary sandbox implementing `vn::llm::IRawHttpTransport`.
// Parses the real Gemini v1beta wire payload produced by `vn::llm_client` and
// returns raw Gemini v1beta HTTP JSON responses with zero network I/O.
class LoopbackGeminiHttpTransport final : public vn::llm::IRawHttpTransport {
public:
    vn::llm::RawHttpResponse post_json(std::string_view target_path,
                                       std::string_view json_body,
                                       int /*connect_timeout_sec*/,
                                       int /*read_timeout_sec*/) override {
        target_paths.emplace_back(target_path);
        request_bodies.emplace_back(json_body);

        const std::size_t call_idx = call_count++;
        const QueuedHttpOutcome spec =
            call_idx < queue.size() ? queue[call_idx] : QueuedHttpOutcome{};
        if (spec.status_code != kHttpOk) {
            return vn::llm::RawHttpResponse{spec.status_code, spec.raw_error_body};
        }

        const bj::object req = bj::parse(json_body).as_object();
        const std::string prompt(req.at("contents")
                                     .as_array()
                                     .at(0)
                                     .as_object()
                                     .at("parts")
                                     .as_array()
                                     .at(0)
                                     .as_object()
                                     .at("text")
                                     .as_string());

        const std::string open_tag = "<lines_to_translate>\n";
        const std::string close_tag = "</lines_to_translate>";
        const std::size_t start = prompt.find(open_tag);
        const std::size_t content_start =
            start == std::string::npos ? std::string::npos : start + open_tag.size();
        const std::size_t end =
            content_start == std::string::npos ? std::string::npos
                                               : prompt.find(close_tag, content_start);
        std::size_t lines = 0u;
        if (content_start != std::string::npos && end != std::string::npos) {
            for (char c : prompt.substr(content_start, end - content_start)) {
                if (c == '\n') {
                    ++lines;
                }
            }
        }

        std::string numbered_en;
        for (std::size_t i = 1u; i <= lines; ++i) {
            numbered_en += std::to_string(i) + ". English_http" +
                           std::to_string(call_count) + "_line" +
                           std::to_string(i) + ".\n";
        }

        bj::object resp;
        resp["candidates"] = bj::array{
            bj::object{
                {"content",
                 bj::object{
                     {"role", "model"},
                     {"parts",
                      bj::array{
                          bj::object{{"text", "Reasoning over Adieu context..."},
                                     {"thought", true}},
                          bj::object{{"text", std::move(numbered_en)}},
                      }},
                 }},
                {"finishReason", "STOP"},
            },
        };
        resp["usageMetadata"] = bj::object{
            {"promptTokenCount", kHealthyPromptTokens},
            {"candidatesTokenCount", kHealthyCandidateTokens},
            {"thoughtsTokenCount", spec.thought_tokens},
        };
        return vn::llm::RawHttpResponse{kHttpOk, bj::serialize(resp)};
    }

    std::vector<QueuedHttpOutcome> queue;
    std::size_t call_count = 0u;
    std::vector<std::string> target_paths;
    std::vector<std::string> request_bodies;
};

bj::object MakeEntry(std::string_view text, std::string_view type,
                     std::string_view speaker = {}) {
    bj::object o;
    o["text"] = std::string(text);
    o["type"] = std::string(type);
    o["speaker"] = std::string(speaker);
    return o;
}

std::vector<std::string> MakeBatchJpLines(std::string_view tag, std::size_t count) {
    std::vector<std::string> out;
    out.reserve(count);
    for (std::size_t i = 0u; i < count; ++i) {
        out.push_back("　" + std::string(tag) + "の台詞その" +
                      std::to_string(i + 1u) + "。");
    }
    return out;
}

fs::path ResolveCompiledTranslateExe() {
    const fs::path test_dir = fs::u8path(apps::exe_dir());
    const std::vector<fs::path> candidates = {
        test_dir / "02_translate.exe",
        test_dir.parent_path() / "02_translate.exe",
        test_dir.parent_path().parent_path() / "Release" / "02_translate.exe",
        test_dir.parent_path().parent_path() / "install" / "bin" / "02_translate.exe",
    };
    for (const fs::path& cand : candidates) {
        std::error_code ec;
        if (fs::is_regular_file(cand, ec)) {
            return cand;
        }
    }
    return {};
}

}  // namespace

TEST(TranslateCt,
     StrikeOneZeroThoughtRefusedStrikeTwoAbortsFlushingBatchOneThenRecoveryAndReplayIdempotence) {
    ScopedTempDir sandbox;
    const fs::path project_dir = sandbox.path() / "project";
    const fs::path out_dir = project_dir / "script_output";
    fs::create_directories(out_dir);

    const std::vector<std::string> batch1_jps = MakeBatchJpLines("第一幕", kBatchLineCount);
    const std::vector<std::string> batch2_jps = MakeBatchJpLines("第二幕", kBatchLineCount);
    const std::vector<std::string> batch3_jps = MakeBatchJpLines("第三幕", kBatchLineCount);

    bj::array start_arr;
    start_arr.push_back(MakeEntry(kNamaeA, "other"));
    start_arr.push_back(MakeEntry(kHitomiJp, "name"));
    start_arr.push_back(MakeEntry(kNamaeB, "other"));
    start_arr.push_back(MakeEntry(kKousukeJp, "name"));

    bj::array seen900_arr;
    for (const std::string& jp : batch1_jps) {
        seen900_arr.push_back(MakeEntry(jp, "narrative"));
    }

    bj::array seen020_arr;
    for (const std::string& jp : batch2_jps) {
        seen020_arr.push_back(MakeEntry(jp, "dialogue", kMutsukiJp));
    }
    for (const std::string& jp : batch3_jps) {
        seen020_arr.push_back(MakeEntry(jp, "dialogue", kTonaeJp));
    }

    bj::object extracted;
    extracted[kStartScene] = bj::object{{"strings", std::move(start_arr)}};
    extracted[kPrologueScene] = bj::object{{"strings", std::move(seen900_arr)}};
    extracted[kMainScene] = bj::object{{"strings", std::move(seen020_arr)}};

    TranslateOptions opt;
    opt.batch_size = kComponentBatchSize;
    opt.max_batches = kNoBatchLimit;
    opt.input_file = (out_dir / "extracted_text.json").u8string();
    opt.cache_file = (out_dir / "translation_cache_gemini.json").u8string();
    opt.output_file = (out_dir / "translated_text.json").u8string();
    opt.tsv_file = (project_dir / "translation_table.tsv").u8string();
    opt.test_dir = (project_dir / "test").u8string();
    opt.sleep_fn = [](std::chrono::milliseconds) {};

    write_file_text(opt.input_file, json_pretty(bj::value(std::move(extracted))));
    const Bytes pristine_input_bytes = read_file(opt.input_file);

    // Run 1: Batch 1 healthy (260 thoughts) -> Batch 2 Strike-1 (0 thoughts, must NOT
    // be cached) -> Batch 3 Strike-2 (0 thoughts, trips ZeroThoughtError and flushes
    // Batch 1 cache without writing partial translated_text.json or TSV).
    LoopbackGeminiHttpTransport transport_run1;
    transport_run1.queue = {
        {kHttpOk, kHealthyThoughtTokens, {}},
        {kHttpOk, kZeroThoughtTokens, {}},
        {kHttpOk, kZeroThoughtTokens, {}},
    };
    gemini::ClientOptions client_opts1;
    client_opts1.api_key = "offline-ct-key";
    client_opts1.max_retries = 0;
    client_opts1.zero_thought_max_retries = 0;
    client_opts1.raw_transport = &transport_run1;
    gemini::Client client1(std::move(client_opts1));

    EXPECT_THROW((void)translate_all(opt, client1), gemini::ZeroThoughtError);
    EXPECT_EQ(transport_run1.call_count, 3u);

    // Read-only input untouched; zero `.tmp` leftovers; Batch 1 persisted, Batch 2 & 3 absent.
    EXPECT_EQ(read_file(opt.input_file), pristine_input_bytes);
    EXPECT_FALSE(fs::exists(fs::u8path(opt.cache_file + ".tmp")));
    EXPECT_FALSE(fs::exists(fs::u8path(opt.output_file)));
    EXPECT_FALSE(fs::exists(fs::u8path(opt.tsv_file)));

    const Cache after_strike2 = load_cache(opt.cache_file);
    ASSERT_EQ(batch1_jps.size(), kBatchLineCount);
    for (std::size_t i = 0u; i < batch1_jps.size(); ++i) {
        const std::string* en = after_strike2.get(batch1_jps[i]);
        ASSERT_NE(en, nullptr) << "Batch 1 line lost: " << i;
        EXPECT_EQ(*en, "English_http1_line" + std::to_string(i + 1u) + ".");
    }
    ASSERT_EQ(batch2_jps.size(), kBatchLineCount);
    for (const std::string& jp : batch2_jps) {
        EXPECT_EQ(after_strike2.get(jp), nullptr) << "Strike-1 line leaked into cache: " << jp;
    }
    ASSERT_EQ(batch3_jps.size(), kBatchLineCount);
    for (const std::string& jp : batch3_jps) {
        EXPECT_EQ(after_strike2.get(jp), nullptr) << "Strike-2 line leaked into cache: " << jp;
    }

    // Run 2 (Recovery): Provider resumes healthy thinking (`thoughtsTokenCount = 260`).
    // Batch 1 is skipped from disk cache (`0` re-requests), while Batch 2 and Batch 3
    // are translated in 2 HTTP requests, producing `last_translation.json` and TSV.
    LoopbackGeminiHttpTransport transport_run2;
    transport_run2.queue = {
        {kHttpOk, kHealthyThoughtTokens, {}},
        {kHttpOk, kHealthyThoughtTokens, {}},
    };
    gemini::ClientOptions client_opts2;
    client_opts2.api_key = "offline-ct-key";
    client_opts2.max_retries = 0;
    client_opts2.zero_thought_max_retries = 0;
    client_opts2.raw_transport = &transport_run2;
    gemini::Client client2(std::move(client_opts2));

    ASSERT_EQ(translate_all(opt, client2), kExitSuccess);
    ASSERT_EQ(transport_run2.call_count, 2u);
    EXPECT_EQ(transport_run2.request_bodies[0].find(batch1_jps[0]), std::string::npos);

    const fs::path snap_path =
        out_dir / "last_gemini_translate" / "last_translation.json";
    ASSERT_TRUE(fs::is_regular_file(snap_path));
    const bj::object snap = json_parse_file(snap_path.u8string()).as_object();
    EXPECT_EQ(std::string(snap.at("model").as_string()), vn::llm::kDefaultModel);
    EXPECT_EQ(std::string(snap.at("thinking_level").as_string()),
              vn::llm::kDefaultThinkingLevel);
    EXPECT_EQ(snap.at("line_count").as_int64(),
              static_cast<std::int64_t>(kBatchLineCount));
    EXPECT_EQ(snap.at("thought_tokens").as_int64(), kHealthyThoughtTokens);
    EXPECT_EQ(std::string(snap.at("stop_reason").as_string()), "end_turn");

    const Bytes run2_tsv_bytes = read_file(opt.tsv_file);
    const std::string run2_tsv(run2_tsv_bytes.begin(), run2_tsv_bytes.end());
    ASSERT_FALSE(run2_tsv.empty());
    EXPECT_NE(run2_tsv.compare(0, kUtf8BomBytes, "\xEF\xBB\xBF"), 0);
    EXPECT_EQ(run2_tsv.find('\r'), std::string::npos);
    EXPECT_EQ(run2_tsv.find(kNamaeA), std::string::npos);
    EXPECT_EQ(run2_tsv.find(kNamaeB), std::string::npos);
    EXPECT_NE(run2_tsv.find("人見\tHitomi\n"), std::string::npos);
    EXPECT_NE(run2_tsv.find(batch2_jps[0] + "\tEnglish_http1_line1.\n"), std::string::npos);
    EXPECT_NE(run2_tsv.find(batch3_jps[0] + "\tEnglish_http2_line1.\n"), std::string::npos);

    // Run 3 (Idempotent Replay): Removing `last_translation.json` and re-running
    // makes strictly 0 HTTP calls and produces byte-identical TSV & cache.
    std::error_code rm_ec;
    ASSERT_TRUE(fs::remove(snap_path, rm_ec));
    const Bytes run2_cache_bytes = read_file(opt.cache_file);
    ASSERT_EQ(translate_all(opt, client2), kExitSuccess);
    EXPECT_EQ(transport_run2.call_count, 2u);
    EXPECT_FALSE(fs::exists(snap_path));
    EXPECT_EQ(read_file(opt.cache_file), run2_cache_bytes);
    EXPECT_EQ(read_file(opt.tsv_file), run2_tsv_bytes);
    EXPECT_EQ(read_file(opt.input_file), pristine_input_bytes);
}

TEST(TranslateCt,
     Http429RateLimitFollowedByZeroThoughtBatchOnePropagatesThroughRealClientStack) {
    ScopedTempDir sandbox;
    const fs::path out_dir = sandbox.path() / "script_output";
    fs::create_directories(out_dir);

    const std::vector<std::string> batch_jps = MakeBatchJpLines("制限検証", kBatchLineCount);
    bj::array strings;
    for (const std::string& jp : batch_jps) {
        strings.push_back(MakeEntry(jp, "narrative"));
    }
    bj::object extracted;
    extracted[kMainScene] = bj::object{{"strings", std::move(strings)}};

    TranslateOptions opt;
    opt.batch_size = kComponentBatchSize;
    opt.input_file = (out_dir / "extracted_text.json").u8string();
    opt.cache_file = (out_dir / "translation_cache_gemini.json").u8string();
    opt.output_file = (out_dir / "translated_text.json").u8string();
    opt.tsv_file = (sandbox.path() / "translation_table.tsv").u8string();
    opt.test_dir = (sandbox.path() / "test").u8string();

    std::vector<std::int64_t> recorded_sleeps;
    opt.sleep_fn = [&recorded_sleeps](std::chrono::milliseconds d) {
        recorded_sleeps.push_back(d.count());
    };
    write_file_text(opt.input_file, json_pretty(bj::value(std::move(extracted))));

    LoopbackGeminiHttpTransport transport;
    transport.queue = {
        {kHttpTooManyRequests,
         kZeroThoughtTokens,
         R"({"error":{"code":429,"status":"RESOURCE_EXHAUSTED","message":"Quota exceeded"}})"},
        {kHttpOk, kZeroThoughtTokens, {}},
    };
    gemini::ClientOptions client_opts;
    client_opts.api_key = "offline-ct-key";
    client_opts.max_retries = 0;
    client_opts.zero_thought_max_retries = 0;
    client_opts.raw_transport = &transport;
    gemini::Client client(std::move(client_opts));

    EXPECT_THROW((void)translate_all(opt, client), gemini::ZeroThoughtError);
    EXPECT_EQ(transport.call_count, 2u);
    ASSERT_EQ(recorded_sleeps.size(), 1u);
    EXPECT_EQ(recorded_sleeps[0], kExpectedBackoffMs);

    const Cache disk_cache = load_cache(opt.cache_file);
    for (const std::string& jp : batch_jps) {
        EXPECT_EQ(disk_cache.get(jp), nullptr);
    }
    EXPECT_FALSE(fs::exists(fs::u8path(opt.output_file)));
    EXPECT_FALSE(fs::exists(fs::u8path(opt.tsv_file)));
}

TEST(TranslateCt,
     CompiledTranslateCliResumesFullyCachedCorpusOfflineAndRefusesPaidCacheDiscard) {
    const fs::path translate_exe = ResolveCompiledTranslateExe();
    ASSERT_FALSE(translate_exe.empty())
        << "Compiled 02_translate.exe not found near test binary";

    ScopedTempDir sandbox;
    const fs::path project_dir = sandbox.path() / "AdieuProj";
    const fs::path out_dir = project_dir / "script_output";
    fs::create_directories(out_dir);

    bj::object extracted;
    extracted[kStartScene] = bj::object{
        {"strings",
         bj::array{
             MakeEntry(kNamaeA, "other"),
             MakeEntry(kHitomiJp, "name"),
             MakeEntry("　静かな放課後の保健室。", "narrative"),
         }},
    };
    write_file_text((out_dir / "extracted_text.json").u8string(),
                    json_pretty(bj::value(std::move(extracted))));

    Cache paid_cache;
    paid_cache.set("　静かな放課後の保健室。", "A quiet infirmary after school.");
    for (std::size_t i = 0u; i <= CACHE_DISCARD_THRESHOLD; ++i) {
        paid_cache.set("　既訳データ行" + std::to_string(i) + "。",
                       "Paid cached line " + std::to_string(i) + ".");
    }
    const fs::path cache_path = out_dir / "translation_cache_gemini.json";
    save_cache(paid_cache, cache_path.u8string());

    // 1. Offline 100%-cached run via real `02_translate.exe` succeeds with 0 API calls.
    const fs::path tsv_path = project_dir / "translation_table.tsv";
    ASSERT_EQ(apps::run_process_utf8(
                  translate_exe.u8string(),
                  {"--dir", project_dir.u8string(), "--batch", "40", "--out", tsv_path.u8string()}),
              apps::kExitOk);
    EXPECT_FALSE(fs::exists(out_dir / "last_gemini_translate" / "last_translation.json"));
    ASSERT_TRUE(fs::is_regular_file(tsv_path));
    const Bytes tsv_bytes = read_file(tsv_path.u8string());
    const std::string tsv(tsv_bytes.begin(), tsv_bytes.end());
    EXPECT_NE(tsv.find("　静かな放課後の保健室。\tA quiet infirmary after school.\n"),
              std::string::npos);
    EXPECT_EQ(tsv.find(kNamaeA), std::string::npos);

    // 2. Destructive `--retranslate` without `--discard-cache` refuses (`kExitRefused = 2`)
    //    and leaves `translation_cache_gemini.json` byte-for-byte intact.
    const Bytes cache_before_refusal = read_file(cache_path.u8string());
    EXPECT_EQ(apps::run_process_utf8(
                  translate_exe.u8string(),
                  {"--dir", project_dir.u8string(), "--retranslate"}),
              apps::kExitRefused);
    EXPECT_EQ(read_file(cache_path.u8string()), cache_before_refusal);
}

}  // namespace adi::translate
