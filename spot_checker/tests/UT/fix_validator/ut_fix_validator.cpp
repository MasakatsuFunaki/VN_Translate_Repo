// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "checker/fix_validator.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <limits>
#include <string>
#include <string_view>

namespace spot_checker {
namespace {

constexpr std::size_t kPinnedSampleCount = 100;

OlsModel make_wide_ols() {
    OlsModel model;
    model.alpha = 0.0;
    model.beta = 2.0;
    model.sigma = 40.0;
    model.sample_count = kPinnedSampleCount;
    model.valid = true;
    return model;
}

ScriptEntry make_entry(std::string_view speaker,
                       std::string_view jp_text,
                       std::string_view current_en) {
    ScriptEntry entry;
    entry.global_index = 1U;
    entry.script_name = "sc01";
    entry.array_key = "lines";
    entry.entry_index = 1U;
    entry.type = "TEXT";
    entry.text_key = "text";
    entry.speaker = std::string(speaker);
    entry.jp_text = std::string(jp_text);
    entry.current_en = std::string(current_en);
    return entry;
}

Improvement make_candidate(const ScriptEntry& original,
                           std::string_view improved_en,
                           std::string_view category = "mistranslation",
                           double confidence = 0.95,
                           std::string_view error_span = "") {
    Improvement imp;
    imp.global_index = original.global_index;
    imp.script_name = original.script_name;
    imp.entry_index = original.entry_index;
    imp.speaker = original.speaker;
    imp.jp_text = original.jp_text;
    imp.old_en = original.current_en;
    imp.improved_en = std::string(improved_en);
    imp.category = std::string(category);
    imp.severity = "major";
    imp.error_span = std::string(error_span);
    imp.confidence = confidence;
    imp.reason = "Editorial repair.";
    return imp;
}

TEST(FixValidatorTest, EnforcesEmptyPassthroughCategoryConfidencePunctuationAndErrorSpanGates) {
    const OlsModel ols = make_wide_ols();
    const ToneGlossary empty_glossary;

    struct GateCase {
        const char* name;
        std::string_view speaker;
        std::string_view jp_text;
        std::string_view old_en;
        std::string_view new_en;
        std::string_view category;
        double confidence;
        std::string_view error_span;
        bool fix_punctuation;
        ValidationRejection expected_rejection;
    };

    const GateCase cases[] = {
        {"empty_string", "Kenji", "「おはよう。」", "\"Good morning.\"", "",
         "mistranslation", 0.95, "", false, ValidationRejection::EmptyTranslation},
        {"whitespace_only", "Kenji", "「おはよう。」", "\"Good morning.\"", "   \t\r\n  ",
         "mistranslation", 0.95, "", false, ValidationRejection::EmptyTranslation},
        {"speaker_header_with_empty_body", "Kenji", "「おはよう。」",
         "Kenji\r\n\"Good morning.\"", "Kenji\r\n   ",
         "mistranslation", 0.95, "", false, ValidationRejection::EmptyTranslation},
        {"unchanged_translation", "Kenji", "「おはよう。」",
         "\"Good morning.\"", "\"Good morning.\"",
         "mistranslation", 0.95, "", false, ValidationRejection::UntranslatedPassthrough},
        {"candidate_equals_jp", "Kenji", "「おはよう。」",
         "\"Good morning.\"", "「おはよう。」",
         "mistranslation", 0.95, "", false, ValidationRejection::UntranslatedPassthrough},
        {"original_passthrough_locked", "NARRATION", "未翻訳の行", "未翻訳の行",
         "Translated line now.",
         "mistranslation", 0.95, "", false, ValidationRejection::UntranslatedPassthrough},
        {"original_silence_locked", "Yui", "「……」", "\"...\"", "\"......\"",
         "mistranslation", 0.95, "", true, ValidationRejection::UntranslatedPassthrough},
        {"original_engine_token_locked", "NARRATION", "_bg_room_day", "_bg_room_day",
         "Daytime room background.",
         "mistranslation", 0.95, "", false, ValidationRejection::UntranslatedPassthrough},
        {"invalid_category_typo", "Kenji", "「おはよう。」",
         "\"Good morning.\"", "\"Good morning to you.\"",
         "typo", 0.95, "", false, ValidationRejection::InvalidDefectCategory},
        {"confidence_below_seam_0849", "Kenji", "「おはよう。」",
         "\"Good morning.\"", "\"Good morning to you.\"",
         "mistranslation", 0.849, "", false, ValidationRejection::InvalidConfidence},
        {"confidence_above_seam_1001", "Kenji", "「おはよう。」",
         "\"Good morning.\"", "\"Good morning to you.\"",
         "mistranslation", 1.001, "", false, ValidationRejection::InvalidConfidence},
        {"confidence_nan", "Kenji", "「おはよう。」",
         "\"Good morning.\"", "\"Good morning to you.\"",
         "mistranslation", std::numeric_limits<double>::quiet_NaN(), "", false,
         ValidationRejection::InvalidConfidence},
        {"confidence_infinity", "Kenji", "「おはよう。」",
         "\"Good morning.\"", "\"Good morning to you.\"",
         "mistranslation", std::numeric_limits<double>::infinity(), "", false,
         ValidationRejection::InvalidConfidence},
        {"confidence_exact_lower_seam_0850", "Kenji", "「おはよう。」",
         "\"Good morning.\"", "\"Good morning to you.\"",
         "mistranslation", 0.850, "", false, ValidationRejection::None},
        {"confidence_exact_upper_seam_1000", "Kenji", "「おはよう。」",
         "\"Good morning.\"", "\"Good morning to you.\"",
         "stiff_translationese", 1.000, "", false, ValidationRejection::None},
        {"punctuation_only_rejected_when_disabled", "Kenji", "「本当にそうなのか？」",
         "\"Wait, really?\"", "\"Wait... really!\"",
         "character_voice", 0.95, "", false, ValidationRejection::PunctuationOnlyDisabled},
        {"punctuation_only_accepted_when_enabled", "Kenji", "「本当にそうなのか？」",
         "\"Wait, really?\"", "\"Wait... really!\"",
         "character_voice", 0.95, "", true, ValidationRejection::None},
        {"removing_leaked_jp_allowed_without_punct_flag", "Kenji", "「本当にそうなのか？」",
         "\"Wait ああ really?\"", "\"Wait, really?\"",
         "artifact_or_note", 0.95, "", false, ValidationRejection::None},
        {"error_span_unchanged_single", "Kenji", "「昨日、姉に電話した。」",
         "\"He called his sister yesterday.\"", "\"Yesterday, he called his sister.\"",
         "mistranslation", 0.95, "his sister", false, ValidationRejection::ErrorSpanNotChanged},
        {"error_span_unchanged_duplicate_count", "Kenji", "「本当に不思議な出来事だった。」",
         "\"It was very, very strange.\"", "\"Truly, it was very, very odd.\"",
         "stiff_translationese", 0.95, "very", false, ValidationRejection::ErrorSpanNotChanged},
        {"error_span_reduced_duplicate_count_accepted", "Kenji", "「本当に不思議な出来事だった。」",
         "\"It was very, very strange.\"", "\"It was very strange indeed.\"",
         "stiff_translationese", 0.95, "very", false, ValidationRejection::None},
    };

    for (const auto& tc : cases) {
        SCOPED_TRACE(tc.name);
        const ScriptEntry orig = make_entry(tc.speaker, tc.jp_text, tc.old_en);
        const Improvement cand =
            make_candidate(orig, tc.new_en, tc.category, tc.confidence, tc.error_span);
        ValidatorOptions opts;
        opts.min_confidence = 0.85;
        opts.fix_punctuation = tc.fix_punctuation;

        const ValidationResult res =
            validate_proposed_fix(orig, cand, empty_glossary, ols, opts);
        EXPECT_EQ(res.rejection, tc.expected_rejection);
        EXPECT_EQ(res.accepted, tc.expected_rejection == ValidationRejection::None);
    }

    // Runaway > 500-byte punctuation bloat compression is accepted even when fix_punctuation == false:
    const std::string runaway_old = "\"Wait" + std::string(520U, '.') + " really?\"";
    const ScriptEntry runaway_orig =
        make_entry("Kenji", "「本当にそうなのか？」", runaway_old);
    const Improvement runaway_cand =
        make_candidate(runaway_orig, "\"Wait, really?\"", "artifact_or_note", 0.95);
    const ValidationResult runaway_res =
        validate_proposed_fix(runaway_orig, runaway_cand, empty_glossary, ols);
    EXPECT_TRUE(runaway_res.accepted);
    EXPECT_EQ(runaway_res.rejection, ValidationRejection::None);
}

TEST(FixValidatorTest, RejectsLeakedJapaneseCjkPunctuationAndTranslatorRomajiNotes) {
    const OlsModel ols = make_wide_ols();
    const ToneGlossary empty_glossary;

    struct ArtifactCase {
        const char* name;
        std::string_view jp_text;
        std::string_view old_en;
        std::string_view new_en;
        ValidationRejection expected_rejection;
    };

    constexpr ArtifactCase kCases[] = {
        {"leftover_kanji", "「明日は学校へ行こう。」", "\"Let's go tomorrow.\"",
         "\"Let's go to 学校 tomorrow.\"", ValidationRejection::LeakedJapaneseCharacters},
        {"leftover_iteration_mark", "「街の人々が集まった。」", "\"Everyone gathered downtown.\"",
         "\"Town 々 gathered downtown.\"", ValidationRejection::LeakedJapaneseCharacters},
        {"leftover_hiragana", "「風が冷たいな。」", "\"The wind feels chilly.\"",
         "\"The wind の feels chilly.\"", ValidationRejection::LeakedJapaneseCharacters},
        {"leftover_halfwidth_katakana", "「物音がしたぞ。」", "\"There was a sudden noise.\"",
         "\"Heard a sudden ｶﾞﾀｯ noise.\"", ValidationRejection::LeakedJapaneseCharacters},
        {"corner_quote_old_en_allows_clean_corner_quotes", "「おはようございます。」",
         "「Good morning.」", "「Good morning to you.」", ValidationRejection::None},
        {"corner_quote_old_en_rejects_leaked_kana", "「おはようございます。」",
         "「Good morning.」", "「おはよう morning to you.」",
         ValidationRejection::LeakedJapaneseCharacters},
        {"fullwidth_horizontal_bar_u2015", "「待て――行くな！」", "\"Wait, don't go!\"",
         "\"Wait―do not leave yet!\"", ValidationRejection::LeakedFullwidthOrCjkPunctuation},
        {"em_dash_u2014", "「待て――行くな！」", "\"Wait, don't go!\"",
         "\"Wait—do not leave yet!\"", ValidationRejection::LeakedFullwidthOrCjkPunctuation},
        {"unicode_ellipsis_u2026", "「そうか……分かった。」", "\"I see... got it.\"",
         "\"I see… understood now.\"", ValidationRejection::LeakedFullwidthOrCjkPunctuation},
        {"fullwidth_exclamation_uff01", "「危ない！」", "\"Watch out!\"",
         "\"Look out right now！\"", ValidationRejection::LeakedFullwidthOrCjkPunctuation},
        {"ideographic_space_u3000", "「静かな夜だな。」", "\"Such a quiet night.\"",
         "\"Such　a quiet evening.\"", ValidationRejection::LeakedFullwidthOrCjkPunctuation},
        {"introduced_tl_note", "「苦笑するしかなかった。」", "\"He smiled weakly.\"",
         "\"He smiled wryly (TL: bitter smile).\"",
         ValidationRejection::IntroducedTranslatorNote},
        {"introduced_honorific_romaji_gloss", "「待ってよ、お姉ちゃん！」",
         "\"Wait up for me, sis!\"", "\"Wait up, big sister (onee-chan)!\"",
         ValidationRejection::IntroducedTranslatorNote},
        {"introduced_hippu_romaji_gloss", "「腰が机に当たった。」",
         "\"Her hip bumped the desk.\"", "\"Her hips (hippu) bumped the desk.\"",
         ValidationRejection::IntroducedTranslatorNote},
        {"removing_existing_tl_note_accepted", "「苦笑するしかなかった。」",
         "\"He smiled (TL: wry smile).\"", "\"He could only give a wry smile.\"",
         ValidationRejection::None},
    };

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.name);
        const ScriptEntry orig = make_entry("Kenji", tc.jp_text, tc.old_en);
        const Improvement cand = make_candidate(orig, tc.new_en);
        const ValidationResult res =
            validate_proposed_fix(orig, cand, empty_glossary, ols);
        EXPECT_EQ(res.rejection, tc.expected_rejection);
        EXPECT_EQ(res.accepted, tc.expected_rejection == ValidationRejection::None);
    }
}

TEST(FixValidatorTest, EnforcesQuoteEnvelopesAndExactEnginePlaceholderSequence) {
    const OlsModel ols = make_wide_ols();
    const ToneGlossary empty_glossary;

    struct SyntaxCase {
        const char* name;
        std::string_view jp_text;
        std::string_view old_en;
        std::string_view new_en;
        ValidationRejection expected_rejection;
    };

    constexpr SyntaxCase kCases[] = {
        // Quote envelope gates:
        {"dropped_ascii_quotes", "「おはよう。」", "\"Good morning.\"",
         "Good morning to everyone.", ValidationRejection::QuoteEnvelopeMismatch},
        {"unbalanced_interior_ascii_quote", "「おはよう。」", "\"Good morning.\"",
         "\"Good \"morning to everyone.\"", ValidationRejection::QuoteEnvelopeMismatch},
        {"dropped_corner_quotes", "「おはよう。」", "「Good morning.」",
         "Good morning to everyone.", ValidationRejection::QuoteEnvelopeMismatch},
        {"thought_parens_replaced_by_quotes", "（まさか、そんなはずは……）",
         "(No way, that can't be...)", "\"No way, that cannot be true...\"",
         ValidationRejection::QuoteEnvelopeMismatch},
        {"thought_parens_preserved", "（まさか、そんなはずは……）",
         "(No way, that can't be...)", "(No way, that cannot be right...)",
         ValidationRejection::None},
        {"narration_wrapped_in_quotes", "冷たい雨が降り続いていた。",
         "Cold rain kept falling.", "\"Cold rain kept falling outside.\"",
         ValidationRejection::QuoteEnvelopeMismatch},
        {"split_box_leading_quote_closed", "「今日からよろしくな、",
         "\"Nice to meet you,", "\"Pleased to meet you today.\"",
         ValidationRejection::QuoteEnvelopeMismatch},
        {"split_box_leading_quote_preserved", "「今日からよろしくな、",
         "\"Nice to meet you,", "\"Pleased to meet you,",
         ValidationRejection::None},
        {"split_box_trailing_quote_opened", "これから一緒に頑張ろう」",
         "let's do our best.\"", "\"Let's give it our all.\"",
         ValidationRejection::QuoteEnvelopeMismatch},
        {"split_box_trailing_quote_preserved", "これから一緒に頑張ろう」",
         "let's do our best.\"", "let's give it our all.\"",
         ValidationRejection::None},
        {"restoring_missing_quotes_for_spoken_jp_allowed", "「おはよう。」",
         "Good morning.", "\"Good morning to you.\"",
         ValidationRejection::None},
        {"dropped_leading_newline_escape", "「おはよう。」", "\n\"Good morning.\"",
         "\"Good morning to you.\"", ValidationRejection::QuoteEnvelopeMismatch},
        {"dropped_trailing_wait_escape", "「おはよう。」", "\"Good morning.\"\\@",
         "\"Good morning to you.\"", ValidationRejection::QuoteEnvelopeMismatch},

        // Control token & placeholder gates (interior tokens so quote envelope matches):
        {"missing_percent_s", "「おかえり、%s！」",
         "\"Welcome back, %s!\"", "\"Welcome back home!\"",
         ValidationRejection::ControlTokenMismatch},
        {"hallucinated_percent_s", "「おかえりなさい！」",
         "\"Welcome back home!\"", "\"Welcome back, %s!\"",
         ValidationRejection::ControlTokenMismatch},
        {"missing_percent_d", "「残りは%d個だ。」",
         "\"Only %d remain.\"", "\"Only three remain.\"",
         ValidationRejection::ControlTokenMismatch},
        {"width_specifier_percent_02d_altered", "「第%02d章の始まりだ。」",
         "\"Chapter %02d begins.\"", "\"Chapter %d begins now.\"",
         ValidationRejection::ControlTokenMismatch},
        {"hallucinated_percent_02d", "「第一章の始まりだ。」",
         "\"Chapter one begins.\"", "\"Chapter %02d begins.\"",
         ValidationRejection::ControlTokenMismatch},
        {"swapped_placeholder_order", "「%sは%d点を獲得した。」",
         "\"%s scored %d points.\"", "\"Scored %d points, %s!\"",
         ValidationRejection::ControlTokenMismatch},
        {"missing_dollar_str10", "「$str10に聞いてみよう。」",
         "\"Let's ask $str10 now.\"", "\"Let's ask the captain.\"",
         ValidationRejection::ControlTokenMismatch},
        {"hallucinated_dollar_str10", "「隊長に聞いてみよう。」",
         "\"Let's ask the captain.\"", "\"Let's ask $str10 now.\"",
         ValidationRejection::ControlTokenMismatch},
        {"missing_interior_escaped_newline", "「下がれ！\\n危ないぞ！」",
         "\"Step back!\\nWatch out!\"", "\"Step back and watch out!\"",
         ValidationRejection::ControlTokenMismatch},
        {"hallucinated_interior_escaped_newline", "「下がれ、危ないぞ！」",
         "\"Step back, watch out!\"", "\"Step back!\\nWatch out!\"",
         ValidationRejection::ControlTokenMismatch},
        {"missing_interior_wait_escape", "「待て。\\@誰かいるぞ。」",
         "\"Hold it.\\@ Someone's here.\"", "\"Hold it, someone is here.\"",
         ValidationRejection::ControlTokenMismatch},
        {"hallucinated_interior_wait_escape", "「待て、誰かいるぞ。」",
         "\"Hold it, someone's here.\"", "\"Hold it.\\@ Someone is here.\"",
         ValidationRejection::ControlTokenMismatch},
        {"exact_placeholder_sequence_preserved",
         "「%sは第%02d階層で$str10を見つけた。\\n見事だ！\\@先へ進もう。」",
         "\"%s found $str10 on Floor %02d.\\nNice job!\\@ Let's move.\"",
         "\"%s discovered $str10 on Floor %02d.\\nSplendid work!\\@ Let us proceed.\"",
         ValidationRejection::None},
    };

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.name);
        const ScriptEntry orig = make_entry("Kenji", tc.jp_text, tc.old_en);
        const Improvement cand = make_candidate(orig, tc.new_en);
        const ValidationResult res =
            validate_proposed_fix(orig, cand, empty_glossary, ols);
        EXPECT_EQ(res.rejection, tc.expected_rejection);
        EXPECT_EQ(res.accepted, tc.expected_rejection == ValidationRejection::None);
    }
}

TEST(FixValidatorTest, EnforcesGlossaryPronounDeltaNonRegressionHSkipGuardAndLengthBounds) {
    const OlsModel ols = make_wide_ols();

    ToneGlossary glossary;
    glossary.add_term("白皇学院", "Hakuo Academy", /*is_person_name=*/false);
    glossary.add_term("水瀬名雪", "Nayuki", /*is_person_name=*/true);

    struct DomainCase {
        const char* name;
        std::string_view speaker;
        std::string_view jp_text;
        std::string_view old_en;
        std::string_view new_en;
        ValidationRejection expected_rejection;
    };

    constexpr DomainCase kCases[] = {
        // Glossary gates:
        {"non_person_lore_dropped_even_with_pronoun", "Kenji",
         "「白皇学院の門前で待っている。」",
         "\"Waiting in front of Hakuo Academy.\"",
         "\"Waiting right outside their school gate.\"",
         ValidationRejection::GlossaryTermDropped},
        {"person_name_dropped_without_3p_pronoun", "Kenji",
         "「水瀬名雪はもう帰ったよ。」",
         "\"Nayuki already went home.\"",
         "\"That student already went home.\"",
         ValidationRejection::GlossaryTermDropped},
        {"person_name_dropped_with_only_1p_2p_pronouns", "Kenji",
         "「水瀬名雪はもう帰ったよ。」",
         "\"Nayuki already went home.\"",
         "\"I heard you already went home.\"",
         ValidationRejection::GlossaryTermDropped},
        {"person_name_dropped_without_increasing_existing_3p_count", "Kenji",
         "「水瀬名雪は傘を忘れた。」",
         "\"Nayuki forgot her umbrella.\"",
         "\"That girl forgot her umbrella.\"",
         ValidationRejection::GlossaryTermDropped},
        {"person_name_substituted_by_new_3p_pronoun_0_to_1", "Kenji",
         "「水瀬名雪はもう帰ったよ。」",
         "\"Nayuki already went home.\"",
         "\"She already went home today.\"",
         ValidationRejection::None},
        {"person_name_substituted_by_additional_3p_pronoun_1_to_2", "Kenji",
         "「水瀬名雪は傘を忘れた。」",
         "\"Nayuki forgot her umbrella.\"",
         "\"She forgot her umbrella today.\"",
         ValidationRejection::None},

        // Non-regression & H-lexicon bypass guard (`apply_h_skip = false`):
        {"dropped_yakuwarigo_nya_rejected", "Tama",
         "「今日はいい天気だにゃ。」",
         "\"Nice weather today, nya.\"",
         "\"The weather sure is nice today.\"",
         ValidationRejection::WeaknessScoreRegression},
        {"score_regression_above_tolerance_on_flawed_line", "Kenji",
         "「俺を助けてくれたんだ。」",
         "\"Someone came to the rescue back then.\"",
         "\"I rescued you back then instead.\"",
         ValidationRejection::WeaknessScoreRegression},
        {"injected_h_word_does_not_bypass_non_regression", "Tama",
         "「俺は絶対に諦めないにゃ。」",
         "\"I will never give up, nya.\"",
         "\"Giving up on cock is impossible.\"",
         ValidationRejection::WeaknessScoreRegression},
        {"h_speaker_still_checked_with_apply_h_skip_false", "美咲（Ｈ）",
         "「今日はいい天気だにゃ。」",
         "\"Nice weather today, nya.\"",
         "\"The weather sure is nice today.\"",
         ValidationRejection::WeaknessScoreRegression},
        {"restoring_dropped_first_person_improves_score", "Kenji",
         "「俺は絶対に諦めないぞ。」",
         "\"Giving up is out of the question.\"",
         "\"I refuse to give up no matter what.\"",
         ValidationRejection::None},

        // Extreme length distortion gates (passing Gate 13 via wide sigma = 40.0):
        // 3x expansion seam on 48-byte old_en: 144 bytes (== 3x) accepted, 145 bytes (> 140 && > 3x) rejected.
        {"expansion_exact_3x_144_bytes_accepted", "NARRATION",
         "あいうえおかきくけこさしすせそ",
         "Forty-eight byte clean narration sentence right!",
         "Quiet autumn rain drifted softly across every wet stone courtyard while distant temple bells echoed gently through chilly evening mountain air!!",
         ValidationRejection::None},
        {"expansion_over_3x_145_bytes_rejected", "NARRATION",
         "あいうえおかきくけこさしすせそ",
         "Forty-eight byte clean narration sentence right!",
         "Quiet autumn rain drifted softly across every wet stone courtyard while distant temple bells echoed gently through chilly evening mountain air!!!",
         ValidationRejection::ExtremeLengthDistortion},

        // >50% truncation seam on 40-byte old_en: 20 bytes (exact 50%) accepted, 19 bytes (< 50%) rejected.
        {"truncation_exact_50_percent_20_bytes_accepted", "NARRATION",
         "あいうえおかきくけこさしすせそ",
         "Cold rain fell quietly across the night.",
         "Cold rain fell fast.",
         ValidationRejection::None},
        {"truncation_over_50_percent_19_bytes_rejected", "NARRATION",
         "あいうえおかきくけこさしすせそ",
         "Cold rain fell quietly across the night.",
         "Cold rain fell now.",
         ValidationRejection::ExtremeLengthDistortion},

        // Dropped-clause extreme compression: 20 Hiragana (expected = 40.0, 0.35 * 40 = 14.0),
        // old_en < 30 bytes with leaked JP (before.total_score = 8.0) -> 13-byte new_en fixes JP leak
        // (after.total_score = 3.5 < 8.0 passes Gate 13) and triggers Gate 14 newly dropped clause!
        {"newly_triggered_dropped_clause_compression_rejected", "Kenji",
         "「あいうえおかきくけこさしすせそたちつてと」",
         "\"He came back early あ\"",
         "\"He came in.\"",
         ValidationRejection::ExtremeLengthDistortion},
    };

    // Verify our pinned byte-length BVA test vectors match exact intended byte counts:
    ASSERT_EQ(std::string_view("Forty-eight byte clean narration sentence right!").size(), 48U);
    ASSERT_EQ(std::string_view(
                  "Quiet autumn rain drifted softly across every wet stone courtyard while "
                  "distant temple bells echoed gently through chilly evening mountain air!!")
                  .size(),
              144U);
    ASSERT_EQ(std::string_view(
                  "Quiet autumn rain drifted softly across every wet stone courtyard while "
                  "distant temple bells echoed gently through chilly evening mountain air!!!")
                  .size(),
              145U);
    ASSERT_EQ(std::string_view("Cold rain fell quietly across the night.").size(), 40U);
    ASSERT_EQ(std::string_view("Cold rain fell fast.").size(), 20U);
    ASSERT_EQ(std::string_view("Cold rain fell now.").size(), 19U);

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.name);
        const ScriptEntry orig = make_entry(tc.speaker, tc.jp_text, tc.old_en);
        const Improvement cand = make_candidate(orig, tc.new_en);
        const ValidationResult res = validate_proposed_fix(orig, cand, glossary, ols);
        EXPECT_EQ(res.rejection, tc.expected_rejection);
        EXPECT_EQ(res.accepted, tc.expected_rejection == ValidationRejection::None);
    }
}

}  // namespace
}  // namespace spot_checker
