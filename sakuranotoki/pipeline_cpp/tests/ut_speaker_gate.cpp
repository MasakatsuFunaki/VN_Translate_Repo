// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Speaker-gate unit tests.  The gate stands between a broken corpus and a paid
// run, so the dossier checks are exercised against synthetic corpora and stub
// prompts as well as the shipped dossier invariants.
#include "translate/speaker_gate.h"
#include "common/util.h"
#include "translate/glossary.h"

#include <cstddef>
#include <map>
#include <string>
#include <utility>

#include <boost/json.hpp>
#include <gtest/gtest.h>

namespace {

namespace bj = boost::json;
namespace tr = snt::translate;

constexpr const char* kNaoya = "直哉";
constexpr const char* kMisuzu = "心鈴";
constexpr const char* kShizuru = "静流";
constexpr const char* kHello = "「あ」";
constexpr const char* kRuriwo = "ルリヲ";
constexpr const char* kShouichi = "章一";

constexpr std::size_t kSampleSpokenLines = 3;
constexpr std::size_t kNaoyaSampleLines = 11880;
constexpr std::size_t kNaoyaLeadLines = 13280;
constexpr std::size_t kMisuzuLeadLines = 4833;
constexpr std::size_t kShizuruLeadLines = 2020;

bj::object entry(const char* type, const char* text) {
    bj::object o;
    o["type"] = type;
    o["text"] = text;
    return o;
}

bj::object spoken(const char* text, const char* speaker) {
    bj::object o = entry("dialogue", text);
    if (speaker != nullptr) {
        o["speaker"] = speaker;
        return o;
    }
    o["speaker"] = nullptr;
    return o;
}

// One script holding `n` lines spoken by `speaker`, tagged by the field.
bj::object corpus_with(const char* speaker, std::size_t n) {
    bj::array strings;
    strings.push_back(entry("name", speaker));
    for (std::size_t i = 0; i < n; ++i) {
        strings.push_back(spoken(kHello, speaker));
    }
    bj::object script;
    script["strings"] = std::move(strings);
    bj::object data;
    data["script\\a.ast"] = std::move(script);
    return data;
}

}  // namespace

TEST(SpeakerGate, counts_spoken_lines_per_label) {
    const auto counts = tr::dialogue_lines_by_speaker(corpus_with(kMisuzu, kSampleSpokenLines));
    ASSERT_EQ(counts.size(), 1U);
    EXPECT_EQ(counts.at(kMisuzu), kSampleSpokenLines);
}

// A null field is an answer, not a gap: the line belongs to nobody and must
// not be counted for the plate that happens to be standing over it.
TEST(SpeakerGate, a_null_speaker_field_is_not_counted_for_the_standing_plate) {
    bj::array strings;
    strings.push_back(entry("name", kShizuru));
    strings.push_back(spoken(kHello, kShizuru));
    strings.push_back(spoken(kHello, nullptr));
    bj::object script;
    script["strings"] = std::move(strings);
    bj::object data;
    data["script\\a.ast"] = std::move(script);

    const auto counts = tr::dialogue_lines_by_speaker(data);
    ASSERT_EQ(counts.size(), 1U);
    EXPECT_EQ(counts.at(kShizuru), 1U);
}

// Without a field the plate standing over the line answers instead.
TEST(SpeakerGate, falls_back_to_the_standing_plate_when_no_field_is_present) {
    bj::array strings;
    strings.push_back(entry("name", kShizuru));
    strings.push_back(entry("dialogue", kHello));
    strings.push_back(entry("title", "第一章"));
    strings.push_back(entry("dialogue", kHello));
    bj::object script;
    script["strings"] = std::move(strings);
    bj::object data;
    data["script\\a.ast"] = std::move(script);

    const auto counts = tr::dialogue_lines_by_speaker(data);
    ASSERT_EQ(counts.size(), 1U);
    EXPECT_EQ(counts.at(kShizuru), 1U) << "a title ends the scene the plate belongs to";
}

TEST(SpeakerGate, a_label_under_the_threshold_needs_no_dossier_line) {
    const std::map<std::string, std::size_t> lines{
        {kNaoya, tr::DOSSIER_LINE_THRESHOLD - 1}};
    EXPECT_TRUE(tr::dossier_gaps(lines, "").empty());
}

TEST(SpeakerGate, a_big_speaker_the_dossier_never_names_is_a_gap) {
    const std::map<std::string, std::size_t> lines{{kNaoya, tr::DOSSIER_LINE_THRESHOLD}};
    const auto gaps = tr::dossier_gaps(lines, "- Misuzu (心鈴) -- **Female**.\n");
    ASSERT_EQ(gaps.size(), 1U);
    EXPECT_EQ(gaps[0].label, kNaoya);
    EXPECT_EQ(gaps[0].lines, tr::DOSSIER_LINE_THRESHOLD);
}

// The failure this check exists for: the character is described, but nothing
// tells the model which pronouns to use for hundreds of lines about them.
TEST(SpeakerGate, a_dossier_line_without_a_sex_is_a_gap) {
    const std::map<std::string, std::size_t> lines{{kNaoya, kNaoyaSampleLines}};
    const auto gaps = tr::dossier_gaps(
        lines, "- Naoya (直哉) -- Painter. Use \"I/me/my\".\n");
    ASSERT_EQ(gaps.size(), 1U);
    EXPECT_NE(gaps[0].reason.find("sex"), std::string::npos);
}

TEST(SpeakerGate, an_explicit_sex_marker_closes_the_gap) {
    const std::map<std::string, std::size_t> lines{{kNaoya, kNaoyaSampleLines}};
    EXPECT_TRUE(tr::dossier_gaps(lines,
                                 "- Naoya (直哉) -- **Male**. Use "
                                 "\"I/me/my\".\n")
                    .empty());
}

// The dossier also states a sex by putting several names on one group line.
TEST(SpeakerGate, a_group_line_states_the_sex_of_every_name_on_it) {
    const std::map<std::string, std::size_t> lines{
        {kMisuzu, kMisuzuLeadLines}, {kShizuru, kShizuruLeadLines}};
    EXPECT_TRUE(tr::dossier_gaps(lines,
                                 "- Misuzu (心鈴), Shizuru "
                                 "(静流) -- women. Use she/her.\n")
                    .empty());
}

// The shipped prompt has to satisfy the check it ships with.
TEST(SpeakerGate, the_shipped_dossier_states_a_sex_for_every_lead) {
    const std::string dossier = tr::prompt_dossier();
    ASSERT_FALSE(dossier.empty()) << "the system prompt lost its dossier block";
    const std::map<std::string, std::size_t> lines{
        {kNaoya, kNaoyaLeadLines},
        {kMisuzu, kMisuzuLeadLines},
        {kShizuru, kShizuruLeadLines}};
    const auto gaps = tr::dossier_gaps(lines, dossier);
    for (const auto& g : gaps) {
        ADD_FAILURE() << g.label << ": " << g.reason;
    }
}

// The defect this check exists for, in the spelling that produced it: the
// table says Ruriwo and the prose still says Rurio, so the model is told one
// name and the runtime table carries another.
TEST(SpeakerGate, a_dossier_spelling_the_name_table_disagrees_with_is_reported) {
    const std::string dossier =
        std::string("- Rurio (") + kRuriwo + ", a girl despite the name) -- she/her.\n";
    const auto found = tr::dossier_name_mismatches(dossier);
    ASSERT_EQ(found.size(), 1U);
    EXPECT_EQ(found[0].label, kRuriwo);
    EXPECT_EQ(found[0].dossier, "Rurio");
    EXPECT_EQ(found[0].table, "Ruriwo");

    const auto blank =
        tr::dossier_name_mismatches(std::string("- (") + kRuriwo + ") -- she/her.\n");
    ASSERT_EQ(blank.size(), 1U);
    EXPECT_TRUE(blank[0].dossier.empty());
    EXPECT_EQ(blank[0].table, "Ruriwo");
}

TEST(SpeakerGate, the_spelling_the_name_table_gives_is_accepted) {
    const std::string dossier =
        std::string("- Ruriwo (") + kRuriwo + ", a girl despite the name) -- she/her.\n";
    EXPECT_TRUE(tr::dossier_name_mismatches(dossier).empty());
}

// The prose introduces some people by family name and given name together.
// That is the table's name with a prefix, not a second spelling of it.
TEST(SpeakerGate, a_family_name_in_front_of_the_given_name_still_agrees) {
    EXPECT_TRUE(tr::dossier_name_mismatches(
                    std::string("- Nakamura Shouichi (") + kShouichi + ") -- he/him.\n")
                    .empty());
    const auto found = tr::dossier_name_mismatches(
        std::string("- Nakamura Shoichi (") + kShouichi + ") -- he/him.\n");
    ASSERT_EQ(found.size(), 1U) << "a misspelt given name is not a prefix";
    EXPECT_EQ(found[0].table, "Shouichi");
}

// A label the prose never introduces is not a spelling disagreement.
TEST(SpeakerGate, a_label_the_prose_never_introduces_is_not_a_mismatch) {
    EXPECT_TRUE(tr::dossier_name_mismatches("- Naoya -- **Male**. Use \"I/me/my\".\n").empty());
}

// The shipped prompt has to satisfy this check too, and it is the whole point
// of deriving the prompt's name list from the table.
TEST(SpeakerGate, the_shipped_dossier_spells_every_name_as_the_table_does) {
    const std::string dossier = tr::prompt_dossier();
    ASSERT_FALSE(dossier.empty()) << "the system prompt lost its dossier block";
    for (const auto& m : tr::dossier_name_mismatches(dossier)) {
        ADD_FAILURE() << m.label << ": the dossier writes '" << m.dossier
                      << "', the table writes '" << m.table << "'";
    }
}
