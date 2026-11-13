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
#include "common/coverage.h"
#include "common/util.h"
#include "translate/glossary.h"
#include "translate/speaker_gate.h"
#include "translate/translate_core.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
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
