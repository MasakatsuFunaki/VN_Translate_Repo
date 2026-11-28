// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Clean-checkout component tests exercising cross-component integration across
// snt::translate (prompt_dossier, run_speaker_checks, translate_batch,
// save_cache / load_cache, glossary), snt::count_coverage, and snt::build_tsv
// (run_build) over a Unicode temporary directory without external game files.
#include "build_tsv/build_tsv.h"
#include "cg/cg_pipeline.h"
#include "cg/image.h"
#include "common/coverage.h"
#include "common/util.h"
#include "pfs_fixture.h"
#include "translate/glossary.h"
#include "translate/speaker_gate.h"
#include "translate/translate_core.h"
#include "vn/llm/llm_client.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace bj = boost::json;
namespace bt = snt::build_tsv;
namespace fs = std::filesystem;
namespace tr = snt::translate;

constexpr std::wstring_view kUnicodeTempPrefix = L"桜ノ刻_ct_";
constexpr std::ptrdiff_t kExpectedTabCount = 1;
constexpr int kExitOk = 0;
constexpr int kNoFailedCheck = 0;
constexpr int kLineOne = 1;
constexpr int kLineTwo = 2;
constexpr int kLineThree = 3;
constexpr std::size_t kExpectedLeadDossierLabels = 1;
constexpr std::size_t kMinorSpeakerSampleLines = 1;
constexpr std::size_t kExpectedExtraRuntimeRows = 3;
constexpr std::size_t kStaleDialogueEntryIndex = 1;
constexpr std::int64_t kExpectedDocTotalEntries = 5;
constexpr std::int64_t kExpectedDocTranslatedEntries = 4;
constexpr std::int64_t kExpectedDocIdentityEntries = 0;
constexpr std::int64_t kExpectedDocExemptEntries = 1;

class ScopedTempDir {
public:
    explicit ScopedTempDir(std::wstring_view prefix) {
        static std::atomic<std::uint64_t> seq{0};
        const auto tick = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        const std::wstring leaf = std::wstring(prefix) + std::to_wstring(tick) +
                                  L"_" + std::to_wstring(++seq);
        path_ = fs::temp_directory_path() / fs::path(leaf);
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    ~ScopedTempDir() noexcept {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

bj::object MakeScriptEntry(std::string_view type,
                           std::string_view text,
                           const char* speaker = nullptr) {
    bj::object obj;
    obj["type"] = type;
    obj["text"] = text;
    if (speaker != nullptr) {
        obj["speaker"] = speaker;
    }
    return obj;
}

std::map<std::string, std::string> ReadEmittedTsvTable(const fs::path& tsv_path) {
    std::ifstream in(tsv_path, std::ios::binary);
    EXPECT_TRUE(static_cast<bool>(in)) << tsv_path.u8string();

    std::map<std::string, std::string> table;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        EXPECT_EQ(line.find('\0'), std::string::npos) << line;
        EXPECT_EQ(std::count(line.begin(), line.end(), '\t'), kExpectedTabCount)
            << line;

        const std::size_t tab = line.find('\t');
        if (tab == std::string::npos) {
            continue;
        }
        const std::string jp = line.substr(0, tab);
        const std::string en = line.substr(tab + 1);
        EXPECT_FALSE(jp.empty());
        EXPECT_FALSE(en.empty());
        const auto [it, inserted] = table.emplace(jp, en);
        EXPECT_TRUE(inserted) << "duplicate TSV key emitted: " << jp;
    }
    return table;
}

}  // namespace

TEST(TranslationPipelineRoundtripCt,
     SpeakerGatePassesAndDossierAgreesWithGlossaryAcrossSyntheticExtraction) {
    const std::string dossier = tr::prompt_dossier();
    ASSERT_FALSE(dossier.empty());
    EXPECT_TRUE(tr::dossier_name_mismatches(dossier).empty());

    bj::array strings;
    for (const auto& [jp_speaker, en_speaker] : tr::name_translations_ordered()) {
        (void)en_speaker;
        strings.push_back(MakeScriptEntry("name", jp_speaker));
        const std::size_t line_count = (jp_speaker == "直哉")
                                           ? tr::DOSSIER_LINE_THRESHOLD
                                           : kMinorSpeakerSampleLines;
        for (std::size_t i = 0; i < line_count; ++i) {
            strings.push_back(
                MakeScriptEntry("dialogue", "「おはよう」", jp_speaker.c_str()));
        }
    }

    bj::object script;
    script["strings"] = std::move(strings);
    bj::object extracted;
    extracted["script\\01_01.ast"] = std::move(script);

    const tr::SpeakerCheckReport report = tr::run_speaker_checks(extracted);
    EXPECT_TRUE(report.passed) << report.reason;
    EXPECT_EQ(report.failed_check, kNoFailedCheck);
    EXPECT_EQ(report.speakers_in_data, tr::name_translations_ordered().size());
    EXPECT_EQ(report.glossary_entries, tr::name_translations_ordered().size());
    EXPECT_EQ(report.dossier_labels, kExpectedLeadDossierLabels);
}

TEST(TranslationPipelineRoundtripCt,
     CacheMaterializationCoverageAndBuildTsvRoundtripOnUnicodePath) {
    const ScopedTempDir temp_dir(kUnicodeTempPrefix);
    const fs::path cache_path = temp_dir.path() / fs::u8path("translation_cache_gemini.json");
    const fs::path doc_path = temp_dir.path() / fs::u8path("translated_text.json");
    const fs::path cov_path = temp_dir.path() / fs::u8path("coverage_report.json");
    const fs::path tsv_path = temp_dir.path() / fs::u8path("translations.tsv");

    tr::Cache cache;
    for (const auto& [jp_name, en_name] : tr::name_translations_ordered()) {
        cache.set_default(jp_name, en_name);
    }

    const std::string kProsePinnedJp = "「鳥谷の絵を見に来たのか」";
    const std::string kIndentedNarrativeJp = "　桜の花びらが静かに舞う。";
    const std::string kTabbedSystemJp = "章\t一";
    const std::string kExemptJp = "「…………」";

    std::vector<std::pair<std::string, std::string>> previous_context;
    const std::vector<std::pair<std::string, std::string>> batch = {
        {"Naoya", kProsePinnedJp},
        {"NARRATION", kIndentedNarrativeJp},
        {"NARRATION", kTabbedSystemJp},
    };

    tr::translate_batch(
        cache,
        previous_context,
        batch,
        []() -> std::map<int, std::string> {
            return {
                {kLineOne,
                 tr::unescape_line_breaks(tr::strip_speaker_tags(
                     "[Naoya] \"Did you come to see Toriya's painting?\\nStep inside.\""))},
                {kLineTwo, "The cherry petals dance quietly."},
                {kLineThree, "Chapter\tOne"},
            };
        },
        std::chrono::seconds::zero());

    tr::save_cache(cache, cache_path.u8string());
    const tr::Cache reloaded_cache = tr::load_cache(cache_path.u8string());
    ASSERT_NE(reloaded_cache.get(kProsePinnedJp), nullptr);
    ASSERT_NE(reloaded_cache.get(kIndentedNarrativeJp), nullptr);
    ASSERT_NE(reloaded_cache.get(kTabbedSystemJp), nullptr);

    bj::array doc_strings;
    {
        bj::object speaker_plate = MakeScriptEntry("name", "直哉");
        speaker_plate["translated"] = "Naoya";
        doc_strings.push_back(std::move(speaker_plate));
    }
    {
        bj::object dlg = MakeScriptEntry("dialogue", kProsePinnedJp, "直哉");
        dlg["translated"] = kProsePinnedJp;  // stale JSON filled from reloaded cache
        doc_strings.push_back(std::move(dlg));
    }
    {
        bj::object narr = MakeScriptEntry("narrative", kIndentedNarrativeJp);
        narr["translated"] = *reloaded_cache.get(kIndentedNarrativeJp);
        doc_strings.push_back(std::move(narr));
    }
    {
        bj::object sys = MakeScriptEntry("other", kTabbedSystemJp);
        sys["translated"] = *reloaded_cache.get(kTabbedSystemJp);
        doc_strings.push_back(std::move(sys));
    }
    {
        bj::object exempt = MakeScriptEntry("dialogue", kExemptJp, "直哉");
        exempt["translated"] = kExemptJp;
        doc_strings.push_back(std::move(exempt));
    }

    bj::object script_obj;
    script_obj["strings"] = std::move(doc_strings);
    bj::object doc_root;
    doc_root["script\\01_01.ast"] = std::move(script_obj);
    snt::write_file(doc_path.u8string(), bj::serialize(bj::value(doc_root)));

    ASSERT_EQ(bt::run_build(doc_path.u8string(),
                            cache_path.u8string(),
                            tsv_path.u8string(),
                            /*include_untranslated=*/false,
                            /*flow_breaks=*/false),
              kExitOk);

    const std::map<std::string, std::string> emitted = ReadEmittedTsvTable(tsv_path);
    EXPECT_EQ(emitted.size(),
              tr::name_translations_ordered().size() + kExpectedExtraRuntimeRows);

    for (const auto& [jp_name, en_name] : tr::name_translations_ordered()) {
        ASSERT_EQ(emitted.count(jp_name), 1U) << jp_name;
        EXPECT_EQ(emitted.at(jp_name), en_name);
    }

    ASSERT_EQ(emitted.count(kProsePinnedJp), 1U);
    EXPECT_EQ(emitted.at(kProsePinnedJp),
              "\"Did you come to see Toritani's painting?\\nStep inside.\"");

    ASSERT_EQ(emitted.count(kIndentedNarrativeJp), 1U);
    EXPECT_EQ(emitted.at(kIndentedNarrativeJp),
              "　The cherry petals dance quietly.");

    ASSERT_EQ(emitted.count("章\\t一"), 1U);
    EXPECT_EQ(emitted.at("章\\t一"), "Chapter\\tOne");

    EXPECT_EQ(emitted.count(kExemptJp), 0U);
    for (const auto& [jp_term, en_term] : tr::terminology_translations()) {
        (void)en_term;
        EXPECT_EQ(emitted.count(jp_term), 0U) << jp_term;
    }

    // Verify coverage counting when cache-filled entry is materialized on the document.
    doc_root["script\\01_01.ast"]
        .as_object()["strings"]
        .as_array()[kStaleDialogueEntryIndex]
        .as_object()["translated"] = *reloaded_cache.get(kProsePinnedJp);
    const snt::CoverageReport cov = snt::count_coverage(doc_root, doc_path.u8string());
    EXPECT_EQ(cov.totals.total, kExpectedDocTotalEntries);
    EXPECT_EQ(cov.totals.translated, kExpectedDocTranslatedEntries);
    EXPECT_EQ(cov.totals.identity, kExpectedDocIdentityEntries);
    EXPECT_EQ(cov.totals.exempt, kExpectedDocExemptEntries);
    EXPECT_EQ(cov.totals.translatable(), kExpectedDocTranslatedEntries);

    snt::write_coverage_report(cov, cov_path.u8string());
    const bj::value parsed_cov = snt::json_parse_file(cov_path.u8string());
    EXPECT_TRUE(parsed_cov.is_object());
}

TEST(TranslationPipelineRoundtripCt,
     TranslateAllFlushesPaidCacheWithoutStrikeOneLinesOnZeroThoughtAbortAndResumesCleanly) {
    constexpr int kHttpStatusOk = 200;
    constexpr int kExitFailure = 1;
    constexpr int kBatchSizeSixteen = 16;
    constexpr std::size_t kLinesPerBatch = 16U;
    constexpr std::size_t kTotalBatches = 3U;
    constexpr std::size_t kTotalUniqueDialogueLines = kLinesPerBatch * kTotalBatches;
    constexpr std::size_t kAbortPassHttpCalls = 3U;
    constexpr std::size_t kRecoveryPassHttpCalls = 2U;
    constexpr std::size_t kIdempotentPassHttpCalls = 0U;
    constexpr std::size_t kPreSeededExtraCount = 1U;
    constexpr std::int64_t kPositiveThoughts = 340;
    constexpr std::int64_t kZeroThoughts = 0;

    class StatefulBatchTransport final : public vn::llm::IRawHttpTransport {
    public:
        std::vector<std::int64_t> scripted_thoughts;
        std::size_t call_index = 0U;

        vn::llm::RawHttpResponse post_json(std::string_view /*target_path*/,
                                           std::string_view json_body,
                                           int /*connect_timeout_sec*/,
                                           int /*read_timeout_sec*/) override {
            if (call_index >= scripted_thoughts.size()) {
                throw std::runtime_error("unexpected HTTP transport invocation");
            }
            const std::int64_t thoughts = scripted_thoughts[call_index++];
            const bj::object req = bj::parse(json_body).as_object();
            const std::string prompt(
                req.at("contents").as_array()[0].as_object()
                    .at("parts").as_array()[0].as_object()
                    .at("text").as_string());

            // Count numbered lines inside <lines_to_translate> so each batch gets
            // deterministic per-call English translations.
            std::size_t count = 0U;
            const std::size_t start = prompt.find("<lines_to_translate>\n");
            const std::size_t end = prompt.find("</lines_to_translate>");
            if (start != std::string::npos && end != std::string::npos && end > start) {
                for (std::size_t p = start + std::string_view("<lines_to_translate>\n").size();
                     p < end;) {
                    const std::size_t nl = prompt.find('\n', p);
                    if (nl == std::string::npos || nl > end) {
                        break;
                    }
                    ++count;
                    p = nl + 1U;
                }
            }

            std::string numbered;
            for (std::size_t i = 0; i < count; ++i) {
                numbered += std::to_string(i + 1U) + ". Call" +
                            std::to_string(call_index) + " Line" +
                            std::to_string(i + 1U) + " Toritani.\\n";
            }
            const std::string body =
                std::string(R"json({"candidates":[{"content":{"parts":[)json") +
                (thoughts > 0
                     ? std::string(R"json({"thought":true,"text":"Checking cast voice..."},)json")
                     : std::string{}) +
                R"json({"text":")json" + numbered +
                R"json("}]},"finishReason":"STOP"}],"usageMetadata":{"promptTokenCount":480,"candidatesTokenCount":96,"thoughtsTokenCount":)json" +
                std::to_string(thoughts) + "}}";
            return vn::llm::RawHttpResponse{kHttpStatusOk, body};
        }
    };

    const ScopedTempDir temp_dir(kUnicodeTempPrefix);
    const fs::path input_path = temp_dir.path() / fs::u8path("extracted_text.json");
    const fs::path cache_path = temp_dir.path() / fs::u8path("translation_cache_gemini.json");
    const fs::path output_path = temp_dir.path() / fs::u8path("translated_text.json");
    const fs::path tsv_path = temp_dir.path() / fs::u8path("translations.tsv");

    std::vector<std::string> batch_jp[kTotalBatches];
    bj::array live_strings;
    for (std::size_t b = 0; b < kTotalBatches; ++b) {
        for (std::size_t i = 0; i < kLinesPerBatch; ++i) {
            const std::string jp = "「鳥谷の桜・第" + std::to_string(b + 1U) + "章" +
                                   std::to_string(i + 1U) + "節」";
            batch_jp[b].push_back(jp);
            live_strings.push_back(MakeScriptEntry("dialogue", jp, "直哉"));
        }
    }

    // Build a second roster script whose lines are already pre-cached so the 6-check
    // speaker gate passes 100% without consuming API batches.
    const std::string kRosterCachedJp = "「おはようございます」";
    const std::string kRosterCachedEn = "\"Good morning to you.\"";
    bj::array roster_strings;
    for (const auto& [jp_speaker, en_speaker] : tr::name_translations_ordered()) {
        (void)en_speaker;
        roster_strings.push_back(MakeScriptEntry("name", jp_speaker));
        roster_strings.push_back(
            MakeScriptEntry("dialogue", kRosterCachedJp, jp_speaker.c_str()));
    }
    for (std::size_t i = 0; i < tr::DOSSIER_LINE_THRESHOLD; ++i) {
        roster_strings.push_back(MakeScriptEntry("dialogue", kRosterCachedJp, "直哉"));
    }

    bj::object extracted_root;
    extracted_root["script\\00_live.ast"] = bj::object{
        {"jp_strings", static_cast<std::int64_t>(live_strings.size())},
        {"strings", std::move(live_strings)},
    };
    extracted_root["script\\01_roster.ast"] = bj::object{
        {"jp_strings", static_cast<std::int64_t>(roster_strings.size())},
        {"strings", std::move(roster_strings)},
    };
    const std::string original_extracted_bytes =
        snt::json_pretty(bj::value(extracted_root), 2);
    snt::write_file_text(input_path.u8string(), original_extracted_bytes);

    tr::Cache seeded;
    seeded.set(kRosterCachedJp, kRosterCachedEn);
    tr::save_cache(seeded, cache_path.u8string());

    // Pass 1: Batch 1 thinks (340 tokens -> cached), Batch 2 returns 0 thoughts
    // (Strike 1 -> refused from cache), Batch 3 returns 0 thoughts (Strike 2 ->
    // ZeroThoughtError caught by translate_all, flushing cache to disk and
    // returning nonzero without writing partial translated_text.json / TSV).
    StatefulBatchTransport abort_transport;
    abort_transport.scripted_thoughts = {
        kPositiveThoughts,
        kZeroThoughts,
        kZeroThoughts,
    };

    tr::TranslateOptions opt;
    opt.batch_size = kBatchSizeSixteen;
    opt.input_file = input_path.u8string();
    opt.cache_file = cache_path.u8string();
    opt.output_file = output_path.u8string();
    opt.tsv_file = tsv_path.u8string();
    opt.raw_transport = &abort_transport;

    EXPECT_EQ(tr::translate_all(opt), kExitFailure);
    EXPECT_EQ(abort_transport.call_index, kAbortPassHttpCalls);
    EXPECT_FALSE(fs::exists(output_path));
    EXPECT_FALSE(fs::exists(tsv_path));

    // Read-only input must remain byte-for-byte unmodified.
    const snt::Bytes input_after_pass1 = snt::read_file(input_path.u8string());
    const snt::Bytes expected_input_bytes = [&] {
        const fs::path check = temp_dir.path() / fs::u8path("expected_input.json");
        snt::write_file_text(check.u8string(), original_extracted_bytes);
        const snt::Bytes b = snt::read_file(check.u8string());
        fs::remove(check);
        return b;
    }();
    EXPECT_EQ(input_after_pass1, expected_input_bytes);

    // On-disk cache must preserve Batch 1 + seeded + 84 glossary names, and contain
    // ZERO entries from Strike-1 Batch 2 or Strike-2 Batch 3.
    const tr::Cache flushed = tr::load_cache(cache_path.u8string());
    EXPECT_EQ(flushed.size(),
              tr::name_translations_ordered().size() + kPreSeededExtraCount + kLinesPerBatch);
    ASSERT_EQ(batch_jp[0].size(), kLinesPerBatch);
    for (const std::string& jp : batch_jp[0]) {
        EXPECT_TRUE(flushed.contains(jp)) << "Batch-1 paid line lost on abort: " << jp;
    }
    ASSERT_EQ(batch_jp[1].size(), kLinesPerBatch);
    for (const std::string& jp : batch_jp[1]) {
        EXPECT_FALSE(flushed.contains(jp)) << "Strike-1 Batch-2 line leaked into cache: " << jp;
    }
    ASSERT_EQ(batch_jp[2].size(), kLinesPerBatch);
    for (const std::string& jp : batch_jp[2]) {
        EXPECT_FALSE(flushed.contains(jp)) << "Strike-2 Batch-3 line leaked into cache: " << jp;
    }

    // Pass 2 (Recovery): upstream reasoning is healthy again; translate_all sends
    // ONLY the 2 uncached batches (32 lines), completes cleanly, and builds TSV.
    StatefulBatchTransport recovery_transport;
    recovery_transport.scripted_thoughts = {
        kPositiveThoughts,
        kPositiveThoughts,
    };
    opt.raw_transport = &recovery_transport;

    ASSERT_EQ(tr::translate_all(opt), kExitOk);
    EXPECT_EQ(recovery_transport.call_index, kRecoveryPassHttpCalls);
    ASSERT_TRUE(fs::exists(output_path));
    ASSERT_TRUE(fs::exists(tsv_path));

    const auto emitted_tsv = ReadEmittedTsvTable(tsv_path);
    EXPECT_EQ(emitted_tsv.size(),
              tr::name_translations_ordered().size() + kPreSeededExtraCount +
                  kTotalUniqueDialogueLines);
    for (std::size_t b = 0; b < kTotalBatches; ++b) {
        ASSERT_EQ(batch_jp[b].size(), kLinesPerBatch);
        for (const std::string& jp : batch_jp[b]) {
            ASSERT_EQ(emitted_tsv.count(jp), 1U) << jp;
        }
    }

    // Pass 3 (Idempotent 0-call resume): with raw_transport cleared (nullptr) and
    // no API key needed, re-running translate_all makes 0 HTTP requests.
    StatefulBatchTransport unused_transport;
    opt.raw_transport = &unused_transport;
    ASSERT_EQ(tr::translate_all(opt), kExitOk);
    EXPECT_EQ(unused_transport.call_index, kIdempotentPassHttpCalls);
}

TEST(TranslationPipelineRoundtripCt,
     NarrativeCgScannerDeliversMultimodalPngInlineDataFromPf8AndResumesWithoutApiKey) {
    constexpr int kHttpStatusOk = 200;
    constexpr int kScanWidth = 800;
    constexpr int kScanHeight = 400;
    constexpr int kTinyWidth = 320;
    constexpr int kTinyHeight = 200;
    constexpr std::size_t kRgbChannels = 3U;
    constexpr std::size_t kExpectedVisionCalls = 2U;
    constexpr std::size_t kExpectedPartsPerVisionCall = 2U;
    constexpr std::size_t kExpectedCandidateCount = 1U;
    constexpr std::size_t kExpectedScannedCount = 3U;
    constexpr std::size_t kPngHeaderMinBytes = 24U;
    constexpr double kPoemConfidence = 0.92;

    auto make_solid_png = [&](int w, int h, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        snt::cg::Image img;
        img.width = w;
        img.height = h;
        img.rgb.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * kRgbChannels);
        for (std::size_t p = 0; p < img.rgb.size(); p += kRgbChannels) {
            img.rgb[p] = r;
            img.rgb[p + 1U] = g;
            img.rgb[p + 2U] = b;
        }
        return snt::cg::encode_png(img);
    };

    auto base64_decode = [](std::string_view in) -> snt::Bytes {
        constexpr int kUnmapped = -1;
        std::array<int, 256> lut{};
        lut.fill(kUnmapped);
        const std::string_view alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (std::size_t i = 0; i < alphabet.size(); ++i) {
            lut[static_cast<unsigned char>(alphabet[i])] = static_cast<int>(i);
        }
        snt::Bytes out;
        int val = 0;
        int bits = -8;
        for (unsigned char c : in) {
            if (c == '=') {
                break;
            }
            const int d = lut[c];
            if (d == kUnmapped) {
                continue;
            }
            val = (val << 6) + d;
            bits += 6;
            if (bits >= 0) {
                out.push_back(static_cast<std::uint8_t>((val >> bits) & 0xFF));
                bits -= 8;
            }
        }
        return out;
    };

    class VisionEndpointDouble final : public vn::llm::IRawHttpTransport {
    public:
        std::vector<std::string> captured_bodies;
        std::vector<std::string> scripted_replies;

        vn::llm::RawHttpResponse post_json(std::string_view /*target_path*/,
                                           std::string_view json_body,
                                           int /*connect_timeout_sec*/,
                                           int /*read_timeout_sec*/) override {
            const std::size_t idx = captured_bodies.size();
            captured_bodies.emplace_back(json_body.data(), json_body.size());
            if (idx >= scripted_replies.size()) {
                throw std::runtime_error("unexpected vision HTTP call");
            }
            return vn::llm::RawHttpResponse{kHttpStatusOk, scripted_replies[idx]};
        }
    };

    const ScopedTempDir sandbox(kUnicodeTempPrefix);
    const fs::path game_dir = sandbox.path() / fs::u8path("game");
    const fs::path project_dir = sandbox.path() / fs::u8path("project");
    fs::create_directories(game_dir);
    fs::create_directories(project_dir);

    const snt::Bytes cg_scenery_png = make_solid_png(kScanWidth, kScanHeight, 0x20U, 0x40U, 0x80U);
    const snt::Bytes ev_poem_png = make_solid_png(kScanWidth, kScanHeight, 0xE0U, 0x90U, 0xA0U);
    const snt::Bytes bg_tiny_png = make_solid_png(kTinyWidth, kTinyHeight, 0x10U, 0x10U, 0x10U);

    const fs::path archive_path = game_dir / fs::u8path("sakuranotoki.pfs");
    const snt::Bytes pf8_bytes = snt::test_fixture::make_pf8({
        {"image\\bg\\bg_tiny.png", bg_tiny_png},
        {"image\\cg\\cg02_scenery.png", cg_scenery_png},
        {"image\\ev\\ev01_poem.png", ev_poem_png},
    });
    snt::write_file(archive_path.u8string(), pf8_bytes);

    // Overlay entries are ordered lexicographically by entry path:
    //   1. image\bg\bg_tiny.png (320x200 -> skipped locally before API)
    //   2. image\cg\cg02_scenery.png (800x400 -> HTTP call #1: false)
    //   3. image\ev\ev01_poem.png (800x400 -> HTTP call #2: true, 0.92)
    VisionEndpointDouble endpoint;
    endpoint.scripted_replies = {
        R"json({
            "candidates": [{"content": {"parts": [
                {"thought": true, "text": "Pure landscape, no story text."},
                {"text": "{\n  \"has_narrative_text\": false,\n  \"confidence\": 0.05,\n  \"description\": \"\"\n}"}
            ]}, "finishReason": "STOP"}],
            "usageMetadata": {"promptTokenCount": 410, "candidatesTokenCount": 22, "thoughtsTokenCount": 90}
        })json",
        R"json({
            "candidates": [{"content": {"parts": [
                {"thought": true, "text": "Vertical poem overlaid on cherry blossoms."},
                {"text": "{\n  \"has_narrative_text\": true,\n  \"confidence\": 0.92,\n  \"description\": \"Cherry-blossom opening poem\"\n}"}
            ]}, "finishReason": "STOP"}],
            "usageMetadata": {"promptTokenCount": 410, "candidatesTokenCount": 28, "thoughtsTokenCount": 115}
        })json",
    };

    snt::cg::CgOptions cg_opt;
    cg_opt.game_dir = game_dir.u8string();
    cg_opt.project_dir = project_dir.u8string();
    cg_opt.scan_only = true;
    cg_opt.assume_yes = true;
    cg_opt.raw_transport = &endpoint;

    ASSERT_EQ(snt::cg::run_find_narrative_cg(cg_opt), kExitOk);
    ASSERT_EQ(endpoint.captured_bodies.size(), kExpectedVisionCalls);

    static constexpr std::array<std::uint8_t, 8> kPngSig = {
        0x89U, 'P', 'N', 'G', '\r', '\n', 0x1AU, '\n'};
    for (std::size_t i = 0; i < endpoint.captured_bodies.size(); ++i) {
        SCOPED_TRACE(i);
        const bj::object wire = bj::parse(endpoint.captured_bodies[i]).as_object();
        const bj::array& parts =
            wire.at("contents").as_array()[0].as_object().at("parts").as_array();
        ASSERT_EQ(parts.size(), kExpectedPartsPerVisionCall);
        const bj::object& inline_data = parts[0].as_object().at("inlineData").as_object();
        EXPECT_EQ(inline_data.at("mimeType").as_string(), "image/png");
        const snt::Bytes decoded_png = base64_decode(
            std::string_view(inline_data.at("data").as_string().data(),
                             inline_data.at("data").as_string().size()));
        ASSERT_GE(decoded_png.size(), kPngHeaderMinBytes);
        EXPECT_TRUE(std::equal(kPngSig.begin(), kPngSig.end(), decoded_png.begin()));
        const int be_w = (static_cast<int>(decoded_png[16]) << 24) |
                         (static_cast<int>(decoded_png[17]) << 16) |
                         (static_cast<int>(decoded_png[18]) << 8) |
                         static_cast<int>(decoded_png[19]);
        const int be_h = (static_cast<int>(decoded_png[20]) << 24) |
                         (static_cast<int>(decoded_png[21]) << 16) |
                         (static_cast<int>(decoded_png[22]) << 8) |
                         static_cast<int>(decoded_png[23]);
        EXPECT_EQ(be_w, kScanWidth);
        EXPECT_EQ(be_h, kScanHeight);
        EXPECT_NE(std::string_view(parts[1].as_object().at("text").as_string().data(),
                                   parts[1].as_object().at("text").as_string().size())
                      .find("has_narrative_text"),
                  std::string_view::npos);
    }

    // Read-only PF8 archive must remain byte-for-byte untouched.
    EXPECT_EQ(snt::read_file(archive_path.u8string()), pf8_bytes);

    const fs::path candidates_path =
        project_dir / fs::u8path("script_output") / fs::u8path("narrative_candidates.json");
    const fs::path scanned_path =
        project_dir / fs::u8path("script_output") / fs::u8path("narrative_scanned.json");
    ASSERT_TRUE(fs::exists(candidates_path));
    ASSERT_TRUE(fs::exists(scanned_path));

    const bj::array candidates = snt::json_parse_file(candidates_path.u8string()).as_array();
    ASSERT_EQ(candidates.size(), kExpectedCandidateCount);
    EXPECT_EQ(candidates[0].as_object().at("arc").as_string(), "sakuranotoki.pfs");
    EXPECT_EQ(candidates[0].as_object().at("entry").as_string(), "image\\ev\\ev01_poem.png");
    EXPECT_DOUBLE_EQ(candidates[0].as_object().at("confidence").as_double(), kPoemConfidence);
    EXPECT_EQ(candidates[0].as_object().at("description").as_string(),
              "Cherry-blossom opening poem");

    const bj::array scanned = snt::json_parse_file(scanned_path.u8string()).as_array();
    EXPECT_EQ(scanned.size(), kExpectedScannedCount);

    // Resume pass with raw_transport = nullptr and empty GEMINI_API_KEY must
    // skip all 3 already-scanned entries with 0 API calls and exit 0.
    cg_opt.raw_transport = nullptr;
    EXPECT_EQ(snt::cg::run_find_narrative_cg(cg_opt), kExitOk);
}
