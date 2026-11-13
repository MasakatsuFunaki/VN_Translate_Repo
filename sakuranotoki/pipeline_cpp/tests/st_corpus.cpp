// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// System-tier tests validating pipeline invariants over a locally generated
// extracted corpus (`script_output/extracted_text.json`) and built runtime
// translation table (`translations.tsv`), auto-skipping when local extraction
// or TSV artifacts have not been generated yet.
#include "common/coverage.h"
#include "common/util.h"
#include "translate/speaker_gate.h"

#include <boost/json.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

namespace {

namespace bj = boost::json;
namespace fs = std::filesystem;
namespace tr = snt::translate;

constexpr std::size_t kMaxLoggedBadTsvLines = 5;
constexpr std::size_t kBadLinePreviewChars = 60;
constexpr std::ptrdiff_t kExpectedTabCount = 1;
constexpr std::int64_t kExpectedTotalCorpusEntries = 92774;
constexpr std::size_t kExpectedDistinctExemptStrings = 1;
constexpr std::int64_t kExpectedExemptOccurrences = 6;
constexpr std::size_t kExpectedSpeakersInData = 84;
constexpr std::size_t kExpectedGlossaryEntries = 84;
constexpr std::size_t kExpectedSampledDialogues = 50;
constexpr int kNoFailedCheck = 0;

fs::path project_root() {
    return fs::u8path(SNT_PROJECT_DIR);
}

fs::path extracted_corpus_path() {
    return project_root() / "script_output" / "extracted_text.json";
}

fs::path deployed_tsv_path() {
    const fs::path root_tsv = project_root() / "translations.tsv";
    if (fs::exists(root_tsv)) {
        return root_tsv;
    }
    return project_root() / "build" / "install" / "game" / "translations.tsv";
}

}  // namespace

TEST(BuildTsvCorpus, deployed_tsv_has_one_tab_per_line) {
    const fs::path tsv = deployed_tsv_path();
    if (!fs::exists(tsv)) {
        GTEST_SKIP() << "translations.tsv not built yet";
    }
    std::ifstream f(tsv, std::ios::binary);
    std::string line;
    std::size_t ln = 0;
    std::size_t bad = 0;
    while (std::getline(f, line) && bad < kMaxLoggedBadTsvLines) {
        ++ln;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        if (std::count(line.begin(), line.end(), '\t') != kExpectedTabCount) {
            ++bad;
            GTEST_LOG_(INFO) << "bad line " << ln << ": "
                             << line.substr(0, kBadLinePreviewChars);
        }
    }
    EXPECT_EQ(bad, 0U);
}

TEST(CoverageCorpus, the_exempt_set_over_the_current_extraction) {
    const fs::path extracted = extracted_corpus_path();
    if (!fs::exists(extracted)) {
        GTEST_SKIP() << "extracted_text.json not built yet";
    }

    const bj::value root = snt::json_parse_file(extracted.u8string());
    std::set<std::string> distinct;
    std::int64_t entries = 0;
    std::int64_t total = 0;
    for (const auto& script : root.get_object()) {
        const auto* strings = script.value().get_object().if_contains("strings");
        if (strings == nullptr || !strings->is_array()) {
            continue;
        }
        for (const auto& ev : strings->get_array()) {
            const std::string text(ev.get_object().at("text").get_string());
            ++total;
            if (snt::is_exempt_line(text)) {
                ++entries;
                distinct.insert(text);
            }
        }
    }
    EXPECT_EQ(total, kExpectedTotalCorpusEntries)
        << "corpus changed -- re-measure before trusting the rest";
    EXPECT_EQ(distinct.size(), kExpectedDistinctExemptStrings);
    EXPECT_EQ(entries, kExpectedExemptOccurrences);
    ASSERT_FALSE(distinct.empty());
    EXPECT_EQ(*distinct.begin(), "？？？");
}

TEST(ExtractCorpus, deployed_extracted_json_has_entries) {
    const fs::path p = extracted_corpus_path();
    if (!fs::exists(p)) {
        GTEST_SKIP() << "extracted_text.json not built yet";
    }
    const bj::value root = snt::json_parse_file(p.u8string());
    ASSERT_TRUE(root.is_object());
    bool has_entries = false;
    for (const auto& kv : root.get_object()) {
        if (!kv.value().is_object()) {
            continue;
        }
        if (const auto* s = kv.value().get_object().if_contains("strings");
            s != nullptr && s->is_array() && !s->get_array().empty()) {
            has_entries = true;
        }
    }
    EXPECT_TRUE(has_entries) << "no archives with a non-empty strings container";
}

TEST(SpeakerGateCorpus, the_deployed_extraction_yields_labels_above_the_threshold) {
    const fs::path p = extracted_corpus_path();
    if (!fs::exists(p)) {
        GTEST_SKIP() << "extracted_text.json not built yet";
    }
    const bj::value root = snt::json_parse_file(p.u8string());
    ASSERT_TRUE(root.is_object());

    const auto counts = tr::dialogue_lines_by_speaker(root.get_object());
    std::size_t above = 0;
    for (const auto& [label, lines] : counts) {
        if (lines >= tr::DOSSIER_LINE_THRESHOLD) {
            ++above;
        }
    }
    EXPECT_GT(above, 0U) << "the per-label line count read nothing from the corpus";

    for (const auto& g : tr::dossier_gaps(counts, tr::prompt_dossier())) {
        ADD_FAILURE() << g.label << " (" << g.lines << " lines): " << g.reason;
    }
}

TEST(TranslateCorpus, the_speaker_gate_passes_over_the_current_extraction) {
    const fs::path extracted = extracted_corpus_path();
    if (!fs::exists(extracted)) {
        GTEST_SKIP() << "extracted_text.json not built yet";
    }

    const bj::value root = snt::json_parse_file(extracted.u8string());
    const tr::SpeakerCheckReport report = tr::run_speaker_checks(root.get_object());
    EXPECT_TRUE(report.passed) << "check " << report.failed_check << ": "
                               << report.reason;
    EXPECT_EQ(report.failed_check, kNoFailedCheck);
    EXPECT_EQ(report.speakers_in_data, kExpectedSpeakersInData);
    EXPECT_EQ(report.glossary_entries, kExpectedGlossaryEntries)
        << "the table and the scripts have to cover each other exactly";
    EXPECT_EQ(report.dialogue_sampled, kExpectedSampledDialogues);
}
