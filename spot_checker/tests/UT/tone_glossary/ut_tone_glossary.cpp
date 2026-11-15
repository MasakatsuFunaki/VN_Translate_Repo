// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "domain/tone_glossary.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>

namespace spot_checker {
namespace {

const GlossaryEntry* find_glossary_entry(const ToneGlossary& glossary,
                                         std::string_view jp_term) {
    const auto& items = glossary.entries();
    const auto it = std::find_if(
        items.begin(), items.end(),
        [jp_term](const GlossaryEntry& entry) { return entry.jp_term == jp_term; });
    if (it == items.end()) {
        return nullptr;
    }
    return &(*it);
}

TEST(ToneGlossary, ParsesMarkdownSectionsStripsFormattingRulesAndEnforcesLengthSeams) {
    constexpr std::string_view kRawMarkdown =
        "# Editorial Tone\n"
        "Keep dialogue crisp and grounded.\n"
        "<formatting_rules>\n"
        "Output strictly a JSON array matching input indices.\n"
        "Never include commentary.\n"
        "</formatting_rules>\n"
        "<name_reference>\n"
        "| Japanese | English |\n"
        "|---|---|\n"
        "| 和倉 賢一 | Kenichi Wakura |\n"
        "| 健一 | Ken |\n"
        "| 太郎 | Taro |\n"
        "| 杏 | Anzu |\n"
        "| 優希 | Yu |\n"
        "</name_reference>\n"
        "<terminology>\n"
        "| 桜ヶ丘学園 | Sakuragaoka |\n"
        "魔導書 = Grimoire\n"
        "聖剣 -> Excalibur\n"
        "一 -> One\n"
        "</terminology>\n";

    const ToneGlossary glossary = ToneGlossary::parse_markdown(kRawMarkdown);

    EXPECT_NE(glossary.sanitized_guide_markdown().find(
                  "Keep dialogue crisp and grounded."),
              std::string::npos);
    EXPECT_EQ(glossary.sanitized_guide_markdown().find("<formatting_rules>"),
              std::string::npos);
    EXPECT_EQ(glossary.sanitized_guide_markdown().find(
                  "Output strictly a JSON array"),
              std::string::npos);

    const std::string stripped_direct =
        ToneGlossary::strip_translator_formatting_rules(kRawMarkdown);
    EXPECT_EQ(stripped_direct.find("<formatting_rules>"), std::string::npos);

    // 1 JP codepoint ('杏') and 2 EN bytes ("Yu") are excluded from entries()
    // while remaining active in speaker_map() / resolve_speaker().
    EXPECT_EQ(find_glossary_entry(glossary, "杏"), nullptr);
    EXPECT_EQ(find_glossary_entry(glossary, "優希"), nullptr);
    EXPECT_EQ(find_glossary_entry(glossary, "一"), nullptr);
    EXPECT_EQ(find_glossary_entry(glossary, "Japanese"), nullptr);
    EXPECT_EQ(glossary.resolve_speaker("杏"), "Anzu");
    EXPECT_EQ(glossary.resolve_speaker("優希"), "Yu");

    // Exact lower boundaries: 2 JP codepoints + 3 EN bytes ("健一" -> "Ken")
    // and 2 JP codepoints + 4 EN bytes ("太郎" -> "Taro").
    const GlossaryEntry* ken = find_glossary_entry(glossary, "健一");
    ASSERT_NE(ken, nullptr);
    EXPECT_EQ(ken->en_term, "Ken");
    EXPECT_TRUE(ken->is_person_name);

    const GlossaryEntry* taro = find_glossary_entry(glossary, "太郎");
    ASSERT_NE(taro, nullptr);
    EXPECT_EQ(taro->en_term, "Taro");
    EXPECT_TRUE(taro->is_person_name);

    const GlossaryEntry* kenichi = find_glossary_entry(glossary, "和倉 賢一");
    ASSERT_NE(kenichi, nullptr);
    EXPECT_EQ(kenichi->en_term, "Kenichi Wakura");
    EXPECT_TRUE(kenichi->is_person_name);

    const GlossaryEntry* school = find_glossary_entry(glossary, "桜ヶ丘学園");
    ASSERT_NE(school, nullptr);
    EXPECT_EQ(school->en_term, "Sakuragaoka");
    EXPECT_FALSE(school->is_person_name);

    const GlossaryEntry* grimoire = find_glossary_entry(glossary, "魔導書");
    ASSERT_NE(grimoire, nullptr);
    EXPECT_EQ(grimoire->en_term, "Grimoire");
    EXPECT_FALSE(grimoire->is_person_name);

    const GlossaryEntry* sword = find_glossary_entry(glossary, "聖剣");
    ASSERT_NE(sword, nullptr);
    EXPECT_EQ(sword->en_term, "Excalibur");
    EXPECT_FALSE(sword->is_person_name);
}

TEST(ToneGlossary, MergesSupplementalSpeakersWithoutOverwritingCanonicalMappings) {
    ToneGlossary glossary = ToneGlossary::parse_markdown(
        "<name_reference>\n"
        "| 和倉 賢一 | Kenichi Wakura |\n"
        "</name_reference>\n");

    glossary.merge_speaker_pair("和倉 賢一", "Wakura");
    EXPECT_EQ(glossary.resolve_speaker("和倉 賢一"), "Kenichi Wakura");

    glossary.merge_speaker_pair("高瀬 瑞希", "Mizuki");
    EXPECT_EQ(glossary.resolve_speaker("  高瀬 瑞希  "), "Mizuki");
    const GlossaryEntry* mizuki = find_glossary_entry(glossary, "高瀬 瑞希");
    ASSERT_NE(mizuki, nullptr);
    EXPECT_EQ(mizuki->en_term, "Mizuki");
    EXPECT_TRUE(mizuki->is_person_name);

    glossary.merge_speaker_pair("凛", "Rin");
    EXPECT_EQ(glossary.resolve_speaker("凛"), "Rin");
    EXPECT_EQ(find_glossary_entry(glossary, "凛"), nullptr);

    const std::size_t count_before_noop = glossary.entries().size();
    glossary.add_term("高瀬 瑞希", "Other", true);
    glossary.add_term("私", "Myself", false);
    glossary.add_term("学園", "Ac", false);
    EXPECT_EQ(glossary.entries().size(), count_before_noop);

    EXPECT_EQ(glossary.resolve_speaker(""), "NARRATION");
    EXPECT_EQ(glossary.resolve_speaker(" \t\r\n "), "NARRATION");
    EXPECT_EQ(glossary.resolve_speaker("  Mystery Boy  "), "Mystery Boy");
}

TEST(ToneGlossary, EnforcesThirdPersonPronounDeltaAndStrictNonPersonPreservation) {
    ToneGlossary glossary;
    glossary.add_term("美咲", "Misaki", true);
    glossary.add_term("健一", "Ken", true);
    glossary.add_term("桜ヶ丘学園", "Sakuragaoka", false);

    struct PreservationCase {
        const char* label;
        std::string_view jp_body;
        std::string_view old_en_body;
        std::string_view new_en_body;
        bool expected_ok;
        std::string_view expected_violated;
    };
    constexpr PreservationCase kCases[] = {
        {"jp_term_absent_allows_omission",
         "朝になった。",
         "Misaki woke up.",
         "Morning arrived.",
         true,
         ""},
        {"old_en_absent_does_not_force_insertion",
         "美咲は笑った。",
         "The girl smiled.",
         "She smiled.",
         true,
         ""},
        {"lore_term_case_insensitive_preserved",
         "桜ヶ丘学園へ向かった。",
         "I walked toward Sakuragaoka.",
         "I walked toward sakuragaoka.",
         true,
         ""},
        {"lore_term_never_excused_by_third_person_pronouns",
         "桜ヶ丘学園へ向かった。",
         "Walked toward Sakuragaoka.",
         "He and she walked toward their school.",
         false,
         "桜ヶ丘学園 -> Sakuragaoka"},
        {"first_and_second_person_pronouns_do_not_excuse_dropped_name",
         "美咲を見た。",
         "Saw Misaki there.",
         "I told you that I saw you there.",
         false,
         "美咲 -> Misaki"},
        {"unchanged_third_person_pronoun_count_rejects_dropped_name",
         "彼は美咲を見た。",
         "He looked at Misaki.",
         "He looked away.",
         false,
         "美咲 -> Misaki"},
        {"pronoun_substrings_inside_words_do_not_count",
         "美咲は去った。",
         "Misaki left.",
         "There was silence in history and theme.",
         false,
         "美咲 -> Misaki"},
        {"name_substring_inside_english_word_rejected",
         "健一が話した。",
         "Ken spoke.",
         "The token broke.",
         false,
         "健一 -> Ken"},
        {"person_name_replaced_by_singular_she_permitted",
         "美咲は微笑んだ。",
         "Misaki smiled softly.",
         "She smiled softly.",
         true,
         ""},
        {"person_name_replaced_when_pronoun_count_increases_one_to_two",
         "彼は美咲に本を渡した。",
         "He handed the book to Misaki.",
         "He handed the book to her.",
         true,
         ""},
        {"person_name_replaced_by_plural_they_permitted",
         "美咲たちは手を振った。",
         "Misaki waved from the gate.",
         "They waved from the gate.",
         true,
         ""},
    };

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.label);
        std::string violated;
        const bool ok = glossary.check_glossary_preservation(
            tc.jp_body, tc.old_en_body, tc.new_en_body, &violated);
        EXPECT_EQ(ok, tc.expected_ok);
        if (!tc.expected_ok) {
            EXPECT_EQ(violated, tc.expected_violated);
        }
    }

    EXPECT_FALSE(glossary.check_glossary_preservation(
        "桜ヶ丘学園へ向かった。",
        "Walked toward Sakuragaoka.",
        "He walked toward school.",
        nullptr));
}

}  // namespace
}  // namespace spot_checker
