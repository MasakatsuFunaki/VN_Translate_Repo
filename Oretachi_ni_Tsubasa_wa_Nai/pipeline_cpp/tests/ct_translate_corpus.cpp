// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki

#include "common/util.h"
#include "translate/gemini_client.h"
#include "translate/glossary.h"
#include "translate/speaker_gate.h"
#include "translate/translate_core.h"
#include "translator_logic.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace bj = boost::json;

constexpr std::size_t kMinExpectedGlossarySpeakers = 323U;
constexpr std::size_t kExpectedCoreCastSpeakers = 19U;
constexpr std::size_t kExpectedInlineTaggedLines = 2U;

bj::value BuildCoreCastCorpusJson() {
    const std::vector<std::string> core_cast = {
        "鷹志",
        "聖騎士ホーク",
        "鷲介",
        "隼人",
        "ヨージ",
        "ガルーダ",
        "翔",
        "小鳩",
        "明日香",
        "日和子",
        "鳴",
        "京",
        "亜衣",
        "狩男",
        "英里子",
        "紀奈子",
        "和馬",
        "春恵",
        "蔵人",
    };

    bj::array s00_strings;
    for (std::size_t i = 0; i < core_cast.size(); ++i) {
        bj::object entry;
        entry["speaker"] = core_cast[i];
        if (i == 0U) {
            entry["text"] = "<10>\\　<\"柳原|やなぎはら\">の街$&寒い";
        } else if (i == 1U) {
            entry["text"] = "「待て！\\行くな！」";
        } else {
            entry["text"] = "「冬の柳原で会おう」";
        }
        s00_strings.push_back(std::move(entry));
    }

    bj::object s00_obj;
    s00_obj["strings"] = std::move(s00_strings);

    bj::object root;
    root["s00_00.sob"] = std::move(s00_obj);
    return root;
}

}  // namespace

TEST(TranslateCorpusTest, GlossaryAndSpeakerGateValidateTrackedToneAndCoreCastCorpus) {
    using namespace oretsuba::translate;

    const fs::path proj_dir = fs::u8path(ORETSUBA_PROJECT_DIR);
    const fs::path tone_path = proj_dir / "TONE_OF_NOVEL.md";
    ASSERT_TRUE(fs::is_regular_file(tone_path)) << tone_path.u8string();

    Glossary glossary;
    ASSERT_TRUE(glossary.LoadFromToneFile(tone_path.u8string()));
    EXPECT_GE(glossary.SpeakerTranslationMap().size(), kMinExpectedGlossarySpeakers);
    EXPECT_EQ(glossary.LookupSpeaker("鷹志").value_or(""), "Takashi");
    EXPECT_EQ(glossary.LookupSpeaker("聖騎士ホーク").value_or(""), "Holy Knight Hawk");
    EXPECT_EQ(glossary.LookupSpeaker("鷲介").value_or(""), "Shuusuke");
    EXPECT_EQ(glossary.LookupSpeaker("隼人").value_or(""), "Hayato");
    EXPECT_EQ(glossary.LookupSpeaker("ヨージ").value_or(""), "Yoji");

    SpeakerGate gate;
    const auto decomp = gate.DecomposeInlineControls("<10>\\　<\"柳原|やなぎはら\">の街$&寒い");
    EXPECT_EQ(decomp.leading_control_prefix, "<10>\\");
    EXPECT_EQ(decomp.clean_text, "　柳原の街$&寒い");
    EXPECT_TRUE(decomp.has_midline_pause);

    const std::string preserved =
        gate.PreserveInlineControls("「待て！$&行くな！」", "\"Wait right there! Don't go!\"");
    EXPECT_NE(preserved.find("$&"), std::string::npos);

    const bj::value corpus = BuildCoreCastCorpusJson();
    const SpeakerCheckReport report = gate.ValidateCorpus(corpus, glossary);
    EXPECT_TRUE(report.passed) << report.reason;
    EXPECT_EQ(report.corpus_speakers, kExpectedCoreCastSpeakers);
    EXPECT_EQ(report.inline_tag_lines, kExpectedInlineTaggedLines);

    bj::object unmapped_entry;
    unmapped_entry["speaker"] = "未登録スピーカー";
    unmapped_entry["text"] = "「テスト」";
    bj::object unmapped_script;
    unmapped_script["strings"] = bj::array{std::move(unmapped_entry)};
    bj::object unmapped_root;
    unmapped_root["s99_99.sob"] = std::move(unmapped_script);

    const SpeakerCheckReport failed_report = gate.ValidateCorpus(unmapped_root, glossary);
    EXPECT_FALSE(failed_report.passed);
    EXPECT_EQ(failed_report.failed_check, 1);
}

namespace {

class ScopedSandboxDir {
public:
    explicit ScopedSandboxDir(std::string_view tag) {
        static std::atomic<unsigned int> counter{0U};
        const unsigned int id = counter.fetch_add(1U, std::memory_order_relaxed);
        dir_ = fs::temp_directory_path() /
               fs::u8path(std::string("oretsuba_ct_") + std::string(tag) + "_" +
                          std::to_string(id));
        std::error_code ec;
        fs::remove_all(dir_, ec);
        fs::create_directories(dir_, ec);
    }

    ~ScopedSandboxDir() {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    ScopedSandboxDir(const ScopedSandboxDir&) = delete;
    ScopedSandboxDir& operator=(const ScopedSandboxDir&) = delete;

    const fs::path& path() const noexcept { return dir_; }

private:
    fs::path dir_;
};

std::string BuildNumberedGeminiWireJson(const std::vector<std::string>& lines,
                                        std::int64_t thoughts_tokens) {
    static constexpr std::int64_t kSyntheticPromptTokens = 480;
    static constexpr std::int64_t kSyntheticCandidateTokens = 120;

    bj::array parts;
    if (thoughts_tokens > 0) {
        parts.push_back(bj::object{
            {"thought", true},
            {"text", "Cross-line register and terminology verification."},
        });
    }
    std::string body;
    for (std::size_t i = 0U; i < lines.size(); ++i) {
        if (i > 0U) {
            body += '\n';
        }
        body += std::to_string(i + 1U) + ". " + lines[i];
    }
    parts.push_back(bj::object{{"text", body}});

    bj::object root;
    root["candidates"] = bj::array{
        bj::object{
            {"content", bj::object{{"role", "model"}, {"parts", std::move(parts)}}},
            {"finishReason", "STOP"},
        },
    };
    root["usageMetadata"] = bj::object{
        {"promptTokenCount", kSyntheticPromptTokens},
        {"candidatesTokenCount", kSyntheticCandidateTokens},
        {"thoughtsTokenCount", thoughts_tokens},
    };
    return bj::serialize(root);
}

std::unordered_map<std::string, std::string> ReadTsvEntriesStrictCp932(const fs::path& tsv_path) {
    const oretsuba::Bytes raw = oretsuba::ReadFileBytes(tsv_path.u8string());
    const std::string text(reinterpret_cast<const char*>(raw.data()), raw.size());
    std::istringstream iss(text);
    std::string line;
    std::unordered_map<std::string, std::string> out;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        const std::size_t tab = line.find('\t');
        EXPECT_NE(tab, std::string::npos) << "Malformed TSV row: " << line;
        if (tab == std::string::npos) {
            continue;
        }
        const std::string jp_col = line.substr(0, tab);
        const std::string en_col = line.substr(tab + 1U);
        const auto jp_cp932 = oretsuba::Utf8ToCp932Strict(jp_col);
        const auto en_cp932 = oretsuba::Utf8ToCp932Strict(en_col);
        EXPECT_TRUE(jp_cp932.has_value()) << jp_col;
        EXPECT_TRUE(en_cp932.has_value()) << en_col;
        if (jp_cp932.has_value()) {
            EXPECT_EQ(oretsuba::Cp932ToUtf8Strict(*jp_cp932).value_or(""), jp_col);
        }
        if (en_cp932.has_value()) {
            EXPECT_EQ(oretsuba::Cp932ToUtf8Strict(*en_cp932).value_or(""), en_col);
        }
        out.emplace(jp_col, en_col);
    }
    return out;
}

bool HasAnyTempArtifacts(const fs::path& root) {
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(root, ec)) {
        if (entry.path().extension() == ".tmp") {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST(TranslateCorpusTest,
     TranslatePipelineRefusesStrikeOneCacheFlushesOnStrikeTwoAndRecoversOnResume) {
    using namespace oretsuba::translate;

    static constexpr unsigned kHttpOk = static_cast<unsigned>(vn::llm::kHttpStatusOk);
    static constexpr int kComponentBatchSize = 16;
    static constexpr std::size_t kBatchLineCount = 16U;
    static constexpr std::size_t kTotalCorpusLines = 48U;
    static constexpr std::size_t kFirstScriptLines = 24U;
    static constexpr std::int64_t kActiveThoughts = 680;
    static constexpr std::size_t kExpectedPass1HttpCalls = 3U;
    static constexpr std::size_t kExpectedPass2HttpCalls = 2U;

    const fs::path repo_proj_dir = fs::u8path(ORETSUBA_PROJECT_DIR);
    const fs::path repo_tone_path = repo_proj_dir / "TONE_OF_NOVEL.md";
    ASSERT_TRUE(fs::is_regular_file(repo_tone_path));

    const ScopedSandboxDir sandbox("strike1_resume");
    const fs::path tone_path = sandbox.path() / "TONE_OF_NOVEL.md";
    const fs::path script_out = sandbox.path() / "script_output";
    const fs::path extracted_path = script_out / "extracted_text.json";
    const fs::path translated_path = script_out / "translated_text.json";
    const fs::path cache_path = script_out / "translation_cache_gemini.json";
    const fs::path last_batch_path = script_out / "last_gemini_translate" / "last_translation.json";
    const fs::path tsv_path = sandbox.path() / "game" / "translation_table.tsv";
    fs::create_directories(script_out);
    fs::create_directories(tsv_path.parent_path());
    fs::copy_file(repo_tone_path, tone_path, fs::copy_options::overwrite_existing);

    const char* const kSpeakers[] = {"鷹志", "小鳩", "明日香", "聖騎士ホーク"};
    constexpr std::size_t kSpeakerCycle = 4U;

    std::vector<std::string> jp_lines;
    jp_lines.reserve(kTotalCorpusLines);
    bj::array s01_strings;
    bj::array s02_strings;
    for (std::size_t i = 0U; i < kTotalCorpusLines; ++i) {
        std::string jp;
        if (i == 0U) {
            jp = "<10>\\「お兄ちゃん、<\"柳原|やなぎはら\">のクレープ屋へ行こうよ！$&急ごう！」";
        } else {
            jp = "「冬の柳原の台詞番号" + std::to_string(i + 1U) + "番目だよ」";
        }
        jp_lines.push_back(jp);
        bj::object row{
            {"type", "dialogue"},
            {"speaker", kSpeakers[i % kSpeakerCycle]},
            {"text", jp},
        };
        if (i < kFirstScriptLines) {
            s01_strings.push_back(std::move(row));
        } else {
            s02_strings.push_back(std::move(row));
        }
    }

    bj::object extracted_root;
    extracted_root["s01_01a.sob"] = bj::object{{"file", "s01_01a.sob"}, {"strings", std::move(s01_strings)}};
    extracted_root["s01_02a.sob"] = bj::object{{"file", "s01_02a.sob"}, {"strings", std::move(s02_strings)}};
    const std::string extracted_bytes = oretsuba::JsonPretty(extracted_root) + "\n";
    oretsuba::WriteFileBytes(extracted_path.u8string(), extracted_bytes);

    TranslateOptions opts;
    opts.batch_size = kComponentBatchSize;
    opts.project_dir = sandbox.path().u8string();
    opts.extracted_file = extracted_path.u8string();
    opts.translated_file = translated_path.u8string();
    opts.cache_file = cache_path.u8string();
    opts.tsv_file = tsv_path.u8string();
    opts.last_batch_log_file = last_batch_path.u8string();
    opts.tone_file = tone_path.u8string();

    // Pass 1: Batch 1 (lines 1..16) succeeds with thoughts > 0; Batch 2 (lines 17..32) returns
    // unblocked 0 thoughts (Strike 1 -> must NOT be cached and must NOT trigger fallback retries);
    // Batch 3 (lines 33..48) returns unblocked 0 thoughts (Strike 2 -> ZeroThoughtError abort,
    // flushing Batch 1's paid cache to disk).
    std::size_t pass1_http_calls = 0U;
    GeminiClient pass1_client(
        "synthetic-ct-key",
        kDefaultModel,
        sandbox.path().u8string(),
        [&](std::string_view /*target*/,
            std::string_view /*request_body*/,
            int /*timeout_sec*/) -> HttpResponse {
            ++pass1_http_calls;
            std::vector<std::string> batch_en;
            batch_en.reserve(kBatchLineCount);
            for (std::size_t i = 0U; i < kBatchLineCount; ++i) {
                if (pass1_http_calls == 1U && i == 0U) {
                    batch_en.push_back(
                        "\"Big Brother, let's visit Yanagihara's crêpe café!$&Hurry up!\"");
                } else {
                    batch_en.push_back(
                        "\"Pass 1 call " + std::to_string(pass1_http_calls) +
                        " line " + std::to_string(i + 1U) + ".\"");
                }
            }
            const std::int64_t thoughts = (pass1_http_calls == 1U) ? kActiveThoughts : 0;
            return HttpResponse{kHttpOk, BuildNumberedGeminiWireJson(batch_en, thoughts)};
        });

    TranslateCore pass1_core(&pass1_client);
    EXPECT_EQ(pass1_core.Run(opts), kExitError);
    EXPECT_EQ(pass1_http_calls, kExpectedPass1HttpCalls);
    EXPECT_FALSE(HasAnyTempArtifacts(sandbox.path()));

    // Read-only input must remain byte-for-byte unmodified.
    const oretsuba::Bytes extracted_after_pass1 = oretsuba::ReadFileBytes(extracted_path.u8string());
    EXPECT_EQ(
        std::string(reinterpret_cast<const char*>(extracted_after_pass1.data()),
                    extracted_after_pass1.size()),
        extracted_bytes);

    ASSERT_TRUE(fs::is_regular_file(cache_path));
    const TranslationCache pass1_cache = LoadCache(cache_path.u8string());
    ASSERT_GE(pass1_cache.Size(), kBatchLineCount);
    ASSERT_EQ(jp_lines.size(), kTotalCorpusLines);
    for (std::size_t i = 0U; i < kBatchLineCount; ++i) {
        SCOPED_TRACE(i);
        ASSERT_NE(pass1_cache.Find(jp_lines[i]), nullptr);
    }
    EXPECT_EQ(*pass1_cache.Find(jp_lines[0]),
              "\"Big Brother, let's visit Yanagihara's crepe cafe!$&Hurry up!\"");
    EXPECT_EQ(*pass1_cache.Find(jp_lines[1]), "\"Pass 1 call 1 line 2.\"");
    for (std::size_t i = kBatchLineCount; i < kTotalCorpusLines; ++i) {
        SCOPED_TRACE(i);
        EXPECT_EQ(pass1_cache.Find(jp_lines[i]), nullptr)
            << "Strike-1 or Strike-2 0-thought line leaked into disk cache at index " << i;
    }

    // Pass 2: Provider reasoning recovers -> resumes cleanly from disk cache, translating only
    // the 32 uncached lines across exact 2 batches and building strict CP932 TSV.
    std::size_t pass2_http_calls = 0U;
    GeminiClient pass2_client(
        "synthetic-ct-key",
        kDefaultModel,
        sandbox.path().u8string(),
        [&](std::string_view /*target*/,
            std::string_view /*request_body*/,
            int /*timeout_sec*/) -> HttpResponse {
            ++pass2_http_calls;
            std::vector<std::string> batch_en;
            batch_en.reserve(kBatchLineCount);
            for (std::size_t i = 0U; i < kBatchLineCount; ++i) {
                batch_en.push_back(
                    "\"Recovered batch " + std::to_string(pass2_http_calls) +
                    " line " + std::to_string(i + 1U) + " • naïve façade.\"");
            }
            return HttpResponse{kHttpOk, BuildNumberedGeminiWireJson(batch_en, kActiveThoughts)};
        });

    TranslateCore pass2_core(&pass2_client);
    ASSERT_EQ(pass2_core.Run(opts), kExitOk);
    EXPECT_EQ(pass2_http_calls, kExpectedPass2HttpCalls);
    EXPECT_FALSE(HasAnyTempArtifacts(sandbox.path()));

    const TranslationCache pass2_cache = LoadCache(cache_path.u8string());
    ASSERT_GE(pass2_cache.Size(), kTotalCorpusLines);
    for (std::size_t i = 0U; i < kTotalCorpusLines; ++i) {
        SCOPED_TRACE(i);
        const std::string* en = pass2_cache.Find(jp_lines[i]);
        ASSERT_NE(en, nullptr);
        EXPECT_TRUE(oretsuba::Utf8ToCp932Strict(*en).has_value()) << *en;
        EXPECT_FALSE(NeedsTranslation(*en)) << *en;
    }
    EXPECT_EQ(*pass2_cache.Find(jp_lines[kBatchLineCount]),
              "\"Recovered batch 1 line 1 * naive facade.\"");

    ASSERT_TRUE(fs::is_regular_file(tsv_path));
    const auto tsv_entries = ReadTsvEntriesStrictCp932(tsv_path);
    ASSERT_GT(tsv_entries.size(), kTotalCorpusLines);
    const std::string normalized_first_key =
        "「お兄ちゃん、柳原のクレープ屋へ行こうよ！$&急ごう！」";
    ASSERT_NE(tsv_entries.find(normalized_first_key), tsv_entries.end());
    EXPECT_EQ(tsv_entries.at(normalized_first_key),
              "\"Big Brother, let's visit Yanagihara's crepe cafe!$&Hurry up!\"");

    // Producer -> real proxy_dll consumer verification: load emitted TSV into
    // oretsuba::proxy::TranslationCatalog and translate raw CP932 SOB records.
    oretsuba::proxy::TranslationCatalog runtime_catalog;
    const std::size_t loaded_rows = runtime_catalog.LoadFromFile(tsv_path.wstring().c_str());
    ASSERT_EQ(loaded_rows, tsv_entries.size());

    const std::string raw_kobato_sob_cp932 =
        oretsuba::proxy::TranslationCatalog::Utf8ToCp932(
            "#小鳩\\KOB0101$&$<10>\\「お兄ちゃん、<\"柳原|やなぎはら\">のクレープ屋へ行こうよ！$&急ごう！」$");
    std::array<char, oretsuba::proxy::kDefaultScratchBufferCapacity> formatted_buf = {};
    ASSERT_TRUE(runtime_catalog.FormatDialogueOrNarration(
        raw_kobato_sob_cp932, formatted_buf.data(), formatted_buf.size()));
    EXPECT_STREQ(
        formatted_buf.data(),
        "#Kobato\\KOB0101$&$<10>\\\"Big Brother, let's visit Yanagihara's crepe cafe!$&Hurry up!\"$");

    const std::string raw_takashi_recovered_cp932 =
        oretsuba::proxy::TranslationCatalog::Utf8ToCp932(
            "#鷹志\\TAK0205$&$" + jp_lines[kBatchLineCount] + "$");
    EXPECT_EQ(
        runtime_catalog.TranslateMessageRecord(raw_takashi_recovered_cp932),
        "#Takashi\\TAK0205$&$\"Recovered batch 1 line 1 * naive facade.\"$");

    // Pass 3: Immediate replay is 100% idempotent with 0 HTTP calls.
    std::size_t pass3_http_calls = 0U;
    GeminiClient pass3_client(
        "synthetic-ct-key",
        kDefaultModel,
        sandbox.path().u8string(),
        [&](std::string_view, std::string_view, int) -> HttpResponse {
            ++pass3_http_calls;
            return HttpResponse{kHttpOk, "{}"};
        });
    TranslateCore pass3_core(&pass3_client);
    ASSERT_EQ(pass3_core.Run(opts), kExitOk);
    EXPECT_EQ(pass3_http_calls, 0U);
    EXPECT_EQ(LoadCache(cache_path.u8string()).values, pass2_cache.values);
}

TEST(TranslateCorpusTest,
     TranslatePipelineEnforcesCp932CacheGateAndSubSliceZeroThoughtImmunity) {
    using namespace oretsuba::translate;

    static constexpr unsigned kHttpOk = static_cast<unsigned>(vn::llm::kHttpStatusOk);
    static constexpr int kComponentBatchSize = 24;
    static constexpr std::size_t kBatch1Size = 24U;
    static constexpr std::size_t kBatch2Size = 24U;
    static constexpr std::size_t kTotalLines = 48U;
    static constexpr std::size_t kInvalidEuroIdx = 21U;
    static constexpr std::size_t kInvalidEmojiIdx = 22U;
    static constexpr std::size_t kInvalidLeakedJpIdx = 23U;
    static constexpr std::int64_t kBatch1Thoughts = 540;

    const fs::path repo_proj_dir = fs::u8path(ORETSUBA_PROJECT_DIR);
    const fs::path repo_tone_path = repo_proj_dir / "TONE_OF_NOVEL.md";
    ASSERT_TRUE(fs::is_regular_file(repo_tone_path));

    const ScopedSandboxDir sandbox("cp932_and_bisect");
    const fs::path tone_path = sandbox.path() / "TONE_OF_NOVEL.md";
    const fs::path script_out = sandbox.path() / "script_output";
    const fs::path extracted_path = script_out / "extracted_text.json";
    const fs::path translated_path = script_out / "translated_text.json";
    const fs::path cache_path = script_out / "translation_cache_gemini.json";
    const fs::path tsv_path = sandbox.path() / "game" / "translation_table.tsv";
    fs::create_directories(script_out);
    fs::create_directories(tsv_path.parent_path());
    fs::copy_file(repo_tone_path, tone_path, fs::copy_options::overwrite_existing);

    std::vector<std::string> jp_lines;
    jp_lines.reserve(kTotalLines);
    bj::array strings;
    for (std::size_t i = 0U; i < kTotalLines; ++i) {
        const std::string jp = "「アレキサンドリアの会話その" + std::to_string(i + 1U) + "」";
        jp_lines.push_back(jp);
        strings.push_back(bj::object{
            {"type", "dialogue"},
            {"speaker", (i % 2U == 0U) ? "鷲介" : "日和子"},
            {"text", jp},
        });
    }

    bj::object extracted_root;
    extracted_root["s02_01a.sob"] = bj::object{
        {"file", "s02_01a.sob"},
        {"strings", std::move(strings)},
    };
    oretsuba::WriteFileBytes(extracted_path.u8string(), oretsuba::JsonPretty(extracted_root) + "\n");

    std::size_t http_calls = 0U;
    GeminiClient client(
        "synthetic-ct-key",
        kDefaultModel,
        sandbox.path().u8string(),
        [&](std::string_view /*target*/,
            std::string_view request_body,
            int /*timeout_sec*/) -> HttpResponse {
            ++http_calls;
            if (http_calls == 1U) {
                std::vector<std::string> b1;
                b1.reserve(kBatch1Size);
                for (std::size_t i = 0U; i < kBatch1Size; ++i) {
                    if (i == kInvalidEuroIdx) {
                        b1.push_back("\"That tab is 500€!\"");
                    } else if (i == kInvalidEmojiIdx) {
                        b1.push_back("\"Wing Road forever 🦅\"");
                    } else if (i == kInvalidLeakedJpIdx) {
                        b1.push_back("\"Welcome to アレキサンドリア!\"");
                    } else {
                        b1.push_back("\"Clean crêpe dialogue #" + std::to_string(i + 1U) + ".\"");
                    }
                }
                return HttpResponse{kHttpOk, BuildNumberedGeminiWireJson(b1, kBatch1Thoughts)};
            }

            // Batch 2 (24 lines): refuse full 24-line requests with PROHIBITED_CONTENT so
            // TranslateBatchWithFallback bisects into neutral sub-slices (20 + 4 lines), which
            // return thoughtsTokenCount = 0 and must succeed without tripping ZeroThoughtError.
            if (request_body.find("24.") != std::string_view::npos) {
                return HttpResponse{
                    kHttpOk,
                    R"json({"candidates":[{"content":{"parts":[]},"finishReason":"PROHIBITED_CONTENT"}]})json",
                };
            }

            static constexpr std::size_t kExpectedFirstSubSliceSize = kFallbackSubBatchSize;
            static constexpr std::size_t kExpectedRemainderSubSliceSize = 4U;
            const std::size_t sub_count =
                (request_body.find("20.") != std::string_view::npos)
                    ? kExpectedFirstSubSliceSize
                    : kExpectedRemainderSubSliceSize;
            std::vector<std::string> sub_en;
            sub_en.reserve(sub_count);
            for (std::size_t i = 0U; i < sub_count; ++i) {
                sub_en.push_back("\"Neutral sub-slice line #" + std::to_string(i + 1U) + ".\"");
            }
            return HttpResponse{kHttpOk, BuildNumberedGeminiWireJson(sub_en, 0)};
        });

    TranslateOptions opts;
    opts.batch_size = kComponentBatchSize;
    opts.project_dir = sandbox.path().u8string();
    opts.extracted_file = extracted_path.u8string();
    opts.translated_file = translated_path.u8string();
    opts.cache_file = cache_path.u8string();
    opts.tsv_file = tsv_path.u8string();
    opts.tone_file = tone_path.u8string();

    TranslateCore core(&client);
    ASSERT_EQ(core.Run(opts), kExitOk);

    const TranslationCache disk_cache = LoadCache(cache_path.u8string());
    EXPECT_EQ(disk_cache.Find(jp_lines[kInvalidEuroIdx]), nullptr);
    EXPECT_EQ(disk_cache.Find(jp_lines[kInvalidEmojiIdx]), nullptr);
    EXPECT_EQ(disk_cache.Find(jp_lines[kInvalidLeakedJpIdx]), nullptr);

    for (std::size_t i = 0U; i < kBatch1Size; ++i) {
        if (i == kInvalidEuroIdx || i == kInvalidEmojiIdx || i == kInvalidLeakedJpIdx) {
            continue;
        }
        SCOPED_TRACE(i);
        ASSERT_NE(disk_cache.Find(jp_lines[i]), nullptr);
    }
    for (std::size_t i = kBatch1Size; i < kBatch1Size + kBatch2Size; ++i) {
        SCOPED_TRACE(i);
        ASSERT_NE(disk_cache.Find(jp_lines[i]), nullptr);
    }

    const bj::value tr_doc = oretsuba::JsonParseFile(translated_path.u8string());
    const auto& out_rows =
        tr_doc.as_object().at("s02_01a.sob").as_object().at("strings").as_array();
    ASSERT_EQ(out_rows.size(), kTotalLines);
    EXPECT_EQ(out_rows[kInvalidEuroIdx].as_object().at("translated").as_string(),
              jp_lines[kInvalidEuroIdx]);
    EXPECT_EQ(out_rows[kInvalidEmojiIdx].as_object().at("translated").as_string(),
              jp_lines[kInvalidEmojiIdx]);
    EXPECT_EQ(out_rows[kInvalidLeakedJpIdx].as_object().at("translated").as_string(),
              jp_lines[kInvalidLeakedJpIdx]);

    const auto tsv_entries = ReadTsvEntriesStrictCp932(tsv_path);
    EXPECT_EQ(tsv_entries.find(jp_lines[kInvalidEuroIdx]), tsv_entries.end());
    EXPECT_EQ(tsv_entries.find(jp_lines[kInvalidEmojiIdx]), tsv_entries.end());
    EXPECT_EQ(tsv_entries.find(jp_lines[kInvalidLeakedJpIdx]), tsv_entries.end());
    EXPECT_NE(tsv_entries.find(jp_lines[kBatch1Size]), tsv_entries.end());

    oretsuba::proxy::TranslationCatalog runtime_catalog;
    ASSERT_EQ(runtime_catalog.LoadFromFile(tsv_path.u8string().c_str()), tsv_entries.size());

    const std::string valid_b1_cp932 =
        oretsuba::proxy::TranslationCatalog::Utf8ToCp932(
            "#鷲介\\SHU0010$&$" + jp_lines[0] + "$");
    EXPECT_EQ(runtime_catalog.TranslateMessageRecord(valid_b1_cp932),
              "#Shuusuke\\SHU0010$&$\"Clean crepe dialogue #1.\"$");

    const std::string valid_b2_subslice_cp932 =
        oretsuba::proxy::TranslationCatalog::Utf8ToCp932(
            "#鷲介\\SHU0025$&$" + jp_lines[kBatch1Size] + "$");
    EXPECT_EQ(runtime_catalog.TranslateMessageRecord(valid_b2_subslice_cp932),
              "#Shuusuke\\SHU0025$&$\"Neutral sub-slice line #1.\"$");

    const std::size_t rejected_indices[] = {
        kInvalidEuroIdx,
        kInvalidEmojiIdx,
        kInvalidLeakedJpIdx,
    };
    for (const std::size_t bad_idx : rejected_indices) {
        SCOPED_TRACE(bad_idx);
        const std::string jp_cp932 =
            oretsuba::proxy::TranslationCatalog::Utf8ToCp932(jp_lines[bad_idx]);
        EXPECT_FALSE(runtime_catalog.LookupTranslation(jp_cp932).has_value());
        const std::string raw_untranslated_record =
            oretsuba::proxy::TranslationCatalog::Utf8ToCp932("#日和子\\HIY0099$&$") +
            jp_cp932 + "$";
        const std::string fallback_rendered =
            runtime_catalog.TranslateMessageRecord(raw_untranslated_record);
        EXPECT_EQ(fallback_rendered, "#Hiyoko\\HIY0099$&$" + jp_cp932 + "$");
        EXPECT_EQ(fallback_rendered.find('?'), std::string::npos);
    }
}
