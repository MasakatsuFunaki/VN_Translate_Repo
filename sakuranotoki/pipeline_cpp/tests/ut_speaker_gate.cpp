// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Speaker-gate tests.  The gate is what stands between a wrong corpus and a
// paid run, so the dossier check is exercised against a stub prompt rather
// than against the shipped one: it must fail for the right reason, not pass
// because the real dossier happens to be complete.
#include <gtest/gtest.h>

#include <filesystem>
#include <map>
#include <string>

#include <boost/json.hpp>

#include "common/util.h"
#include "translate/glossary.h"
#include "translate/speaker_gate.h"

namespace bj = boost::json;
namespace fs = std::filesystem;
using namespace snt::translate;

namespace {

const char* const NAOYA = "\xE7\x9B\xB4\xE5\x93\x89";     // 直哉
const char* const MISUZU = "\xE5\xBF\x83\xE9\x88\xB4";    // 心鈴
const char* const SHIZURU = "\xE9\x9D\x99\xE6\xB5\x81";   // 静流
const char* const HELLO = "\xE3\x80\x8C\xE3\x81\x82\xE3\x80\x8D";  // 「あ」
const char* const RURIWO = "\xE3\x83\xAB\xE3\x83\xAA\xE3\x83\xB2";  // ルリヲ
const char* const SHOUICHI = "\xE7\xAB\xA0\xE4\xB8\x80";            // 章一

bj::object entry(const char* type, const char* text) {
    bj::object o;
    o["type"] = type;
    o["text"] = text;
    return o;
}

bj::object spoken(const char* text, const char* speaker) {
    bj::object o = entry("dialogue", text);
    if (speaker) o["speaker"] = speaker;
    else o["speaker"] = nullptr;
    return o;
}

// One script holding `n` lines spoken by `speaker`, tagged by the field.
bj::object corpus_with(const char* speaker, std::size_t n) {
    bj::array strings;
    strings.push_back(entry("name", speaker));
    for (std::size_t i = 0; i < n; ++i) strings.push_back(spoken(HELLO, speaker));
    bj::object script;
    script["strings"] = std::move(strings);
    bj::object data;
    data["script\\a.ast"] = std::move(script);
    return data;
}

}  // namespace

TEST(SpeakerGate, counts_spoken_lines_per_label) {
    const auto counts = dialogue_lines_by_speaker(corpus_with(MISUZU, 3));
    ASSERT_EQ(counts.size(), 1u);
    EXPECT_EQ(counts.at(MISUZU), 3u);
}

// A null field is an answer, not a gap: the line belongs to nobody and must
// not be counted for the plate that happens to be standing over it.
TEST(SpeakerGate, a_null_speaker_field_is_not_counted_for_the_standing_plate) {
    bj::array strings;
    strings.push_back(entry("name", SHIZURU));
    strings.push_back(spoken(HELLO, SHIZURU));
    strings.push_back(spoken(HELLO, nullptr));
    bj::object script;
    script["strings"] = std::move(strings);
    bj::object data;
    data["script\\a.ast"] = std::move(script);

    const auto counts = dialogue_lines_by_speaker(data);
    ASSERT_EQ(counts.size(), 1u);
    EXPECT_EQ(counts.at(SHIZURU), 1u);
}

// Without a field the plate standing over the line answers instead.
TEST(SpeakerGate, falls_back_to_the_standing_plate_when_no_field_is_present) {
    bj::array strings;
    strings.push_back(entry("name", SHIZURU));
    strings.push_back(entry("dialogue", HELLO));
    strings.push_back(entry("title", "\xE7\xAC\xAC\xE4\xB8\x80\xE7\xAB\xA0"));  // 第一章
    strings.push_back(entry("dialogue", HELLO));
    bj::object script;
    script["strings"] = std::move(strings);
    bj::object data;
    data["script\\a.ast"] = std::move(script);

    const auto counts = dialogue_lines_by_speaker(data);
    ASSERT_EQ(counts.size(), 1u);
    EXPECT_EQ(counts.at(SHIZURU), 1u) << "a title ends the scene the plate belongs to";
}

TEST(SpeakerGate, a_label_under_the_threshold_needs_no_dossier_line) {
    const std::map<std::string, std::size_t> lines{
        {NAOYA, DOSSIER_LINE_THRESHOLD - 1}};
    EXPECT_TRUE(dossier_gaps(lines, "").empty());
}

TEST(SpeakerGate, a_big_speaker_the_dossier_never_names_is_a_gap) {
    const std::map<std::string, std::size_t> lines{{NAOYA, DOSSIER_LINE_THRESHOLD}};
    const auto gaps = dossier_gaps(lines, "- Misuzu (\xE5\xBF\x83\xE9\x88\xB4) -- **Female**.\n");
    ASSERT_EQ(gaps.size(), 1u);
    EXPECT_EQ(gaps[0].label, NAOYA);
    EXPECT_EQ(gaps[0].lines, DOSSIER_LINE_THRESHOLD);
}

// The failure this check exists for: the character is described, but nothing
// tells the model which pronouns to use for hundreds of lines about them.
TEST(SpeakerGate, a_dossier_line_without_a_sex_is_a_gap) {
    const std::map<std::string, std::size_t> lines{{NAOYA, 11880}};
    const auto gaps = dossier_gaps(
        lines, "- Naoya (\xE7\x9B\xB4\xE5\x93\x89) -- Painter. Use \"I/me/my\".\n");
    ASSERT_EQ(gaps.size(), 1u);
    EXPECT_NE(gaps[0].reason.find("sex"), std::string::npos);
}

TEST(SpeakerGate, an_explicit_sex_marker_closes_the_gap) {
    const std::map<std::string, std::size_t> lines{{NAOYA, 11880}};
    EXPECT_TRUE(dossier_gaps(lines,
                             "- Naoya (\xE7\x9B\xB4\xE5\x93\x89) -- **Male**. Use "
                             "\"I/me/my\".\n")
                    .empty());
}

// The dossier also states a sex by putting several names on one group line.
TEST(SpeakerGate, a_group_line_states_the_sex_of_every_name_on_it) {
    const std::map<std::string, std::size_t> lines{{MISUZU, 4833}, {SHIZURU, 2020}};
    EXPECT_TRUE(dossier_gaps(lines,
                             "- Misuzu (\xE5\xBF\x83\xE9\x88\xB4), Shizuru "
                             "(\xE9\x9D\x99\xE6\xB5\x81) -- women. Use she/her.\n")
                    .empty());
}

// The check reads the deployed corpus, not only synthetic ones.  A counter
// that returns nothing here makes the gate report OK on a document it never
// looked at, and no other check crosses this function.
TEST(SpeakerGate, the_deployed_extraction_yields_labels_above_the_threshold) {
    const fs::path p = fs::u8path(std::string(SNT_PROJECT_DIR) +
                                  "\\script_output\\extracted_text.json");
    if (!fs::exists(p)) GTEST_SKIP() << "extracted_text.json not built yet";
    const bj::value root = snt::json_parse_file(p.u8string());
    ASSERT_TRUE(root.is_object());

    const auto counts = dialogue_lines_by_speaker(root.get_object());
    std::size_t above = 0;
    for (const auto& [label, lines] : counts)
        if (lines >= DOSSIER_LINE_THRESHOLD) ++above;
    EXPECT_GT(above, 0u) << "the per-label line count read nothing from the corpus";

    for (const auto& g : dossier_gaps(counts, prompt_dossier()))
        ADD_FAILURE() << g.label << " (" << g.lines << " lines): " << g.reason;
}

// The shipped prompt has to satisfy the check it ships with.
TEST(SpeakerGate, the_shipped_dossier_states_a_sex_for_every_lead) {
    const std::string dossier = prompt_dossier();
    ASSERT_FALSE(dossier.empty()) << "the system prompt lost its dossier block";
    const std::map<std::string, std::size_t> lines{{NAOYA, 13280}, {MISUZU, 4833},
                                                   {SHIZURU, 2020}};
    const auto gaps = dossier_gaps(lines, dossier);
    for (const auto& g : gaps) ADD_FAILURE() << g.label << ": " << g.reason;
}

// The defect this check exists for, in the spelling that produced it: the
// table says Ruriwo and the prose still says Rurio, so the model is told one
// name and the runtime table carries another.
TEST(SpeakerGate, a_dossier_spelling_the_name_table_disagrees_with_is_reported) {
    const std::string dossier =
        std::string("- Rurio (") + RURIWO + ", a girl despite the name) -- she/her.\n";
    const auto found = dossier_name_mismatches(dossier);
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(found[0].label, RURIWO);
    EXPECT_EQ(found[0].dossier, "Rurio");
    EXPECT_EQ(found[0].table, "Ruriwo");

    // A half-edited dossier leaves the label with no English in front of it.
    // That is a disagreement as well, reported with an empty spelling.
    const auto blank =
        dossier_name_mismatches(std::string("- (") + RURIWO + ") -- she/her.\n");
    ASSERT_EQ(blank.size(), 1u);
    EXPECT_TRUE(blank[0].dossier.empty());
    EXPECT_EQ(blank[0].table, "Ruriwo");
}

TEST(SpeakerGate, the_spelling_the_name_table_gives_is_accepted) {
    const std::string dossier =
        std::string("- Ruriwo (") + RURIWO + ", a girl despite the name) -- she/her.\n";
    EXPECT_TRUE(dossier_name_mismatches(dossier).empty());
}

// The prose introduces some people by family name and given name together.
// That is the table's name with a prefix, not a second spelling of it.
TEST(SpeakerGate, a_family_name_in_front_of_the_given_name_still_agrees) {
    EXPECT_TRUE(dossier_name_mismatches(
                    std::string("- Nakamura Shouichi (") + SHOUICHI + ") -- he/him.\n")
                    .empty());
    const auto found = dossier_name_mismatches(
        std::string("- Nakamura Shoichi (") + SHOUICHI + ") -- he/him.\n");
    ASSERT_EQ(found.size(), 1u) << "a misspelt given name is not a prefix";
    EXPECT_EQ(found[0].table, "Shouichi");
}

// A label the prose never introduces is not a spelling disagreement.  Check 6
// is what demands a dossier line, and only above the line threshold.
TEST(SpeakerGate, a_label_the_prose_never_introduces_is_not_a_mismatch) {
    EXPECT_TRUE(dossier_name_mismatches("- Naoya -- **Male**. Use \"I/me/my\".\n").empty());
}

// The shipped prompt has to satisfy this check too, and it is the whole point
// of deriving the prompt's name list from the table.
TEST(SpeakerGate, the_shipped_dossier_spells_every_name_as_the_table_does) {
    const std::string dossier = prompt_dossier();
    ASSERT_FALSE(dossier.empty()) << "the system prompt lost its dossier block";
    for (const auto& m : dossier_name_mismatches(dossier))
        ADD_FAILURE() << m.label << ": the dossier writes '" << m.dossier
                      << "', the table writes '" << m.table << "'";
}
