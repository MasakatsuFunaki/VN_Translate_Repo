// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "detector/weakness_signals.h"

#include "domain/vn_line_syntax.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace spot_checker {
namespace {

constexpr double kFloatTol = 1e-9;
constexpr std::size_t kPinnedSampleCount = 100;

ScriptEntry make_entry(std::size_t index,
                       std::string_view script_name,
                       std::string_view speaker,
                       std::string_view jp_text,
                       std::string_view current_en) {
    ScriptEntry entry;
    entry.global_index = index;
    entry.script_name = std::string(script_name);
    entry.array_key = "lines";
    entry.entry_index = index;
    entry.type = "TEXT";
    entry.text_key = "text";
    entry.speaker = std::string(speaker);
    entry.jp_text = std::string(jp_text);
    entry.current_en = std::string(current_en);
    return entry;
}

OlsModel make_pinned_ols(double alpha, double beta, double sigma) {
    OlsModel model;
    model.alpha = alpha;
    model.beta = beta;
    model.sigma = sigma;
    model.sample_count = kPinnedSampleCount;
    model.valid = true;
    return model;
}

TEST(WeaknessSignalsTest, ComputesHandCalculatedPellegrinoInfoBits) {
    struct InfoBitsCase {
        const char* name;
        std::string_view jp_body;
        double expected_bits;
    };

    constexpr InfoBitsCase kCases[] = {
        {"empty", "", 0.0},
        {"ascii_whitespace_only", " \t\r\n", 0.0},
        {"kanji_with_iteration_mark", "人々", 4.8},
        {"katakana_vs_prolongation_and_interpunct", "カード・キー", 4.20},
        {"halfwidth_katakana_and_prolongation", "ｶｰ", 1.40},
        {"mixed_hiragana_ascii_and_quotes", "「あa1!」", 3.25},
    };

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.name);
        EXPECT_NEAR(compute_script_info_bits(tc.jp_body), tc.expected_bits, kFloatTol);
    }
}

TEST(WeaknessSignalsTest, PreparesKanjiCompoundsCallbackTokensAndContractionStems) {
    const ScriptEntry compound_entry = make_entry(
        0, "sc01", "Kenji",
        "私が約束と人々を守る。",
        "\"That's why I didn't.\"");
    const PreparedLine prep = prepare_line(compound_entry);

    ASSERT_EQ(prep.kanji_compounds.size(), 2U);
    EXPECT_EQ(prep.kanji_compounds[0], "約束");
    EXPECT_EQ(prep.kanji_compounds[1], "人々");
    EXPECT_EQ(prep.en_content_stems.size(), 0U);

    const PreparedLine prep_ore =
        prepare_line(make_entry(1, "sc01", "Kenji", "「俺は……」", "\"I...\""));
    const PreparedLine prep_watashi =
        prepare_line(make_entry(2, "sc01", "Yui", "「私は……」", "\"I...\""));

    EXPECT_EQ(std::count(prep_ore.jp_callback_tokens.begin(),
                         prep_ore.jp_callback_tokens.end(),
                         "俺は"),
              0);
    EXPECT_EQ(std::count(prep_watashi.jp_callback_tokens.begin(),
                         prep_watashi.jp_callback_tokens.end(),
                         "私は"),
              0);
}

TEST(WeaknessSignalsTest, FitsLengthRegressionSampleAndVarianceSeams) {
    const OlsModel empty_model = fit_length_regression({}, /*skip_h_scenes=*/true);
    EXPECT_FALSE(empty_model.valid);
    EXPECT_EQ(empty_model.sample_count, 0U);

    const std::vector<ScriptEntry> three_rows = {
        make_entry(0, "sc01", "NARRATION", "あいうえ", "Fifteen-byte ok"),
        make_entry(1, "sc01", "NARRATION", "あいうえおかきくけこ", "Twenty-five byte sentence"),
        make_entry(2, "sc01", "NARRATION", "あいうえおかきくけこさしすせそたちつ",
                   "Forty-one byte clean narration line here."),
    };
    const OlsModel three_model = fit_length_regression(three_rows, /*skip_h_scenes=*/true);
    EXPECT_FALSE(three_model.valid);
    EXPECT_EQ(three_model.sample_count, 3U);

    const std::vector<ScriptEntry> zero_var_rows = {
        make_entry(0, "sc01", "NARRATION", "あいうえおか", "Twelve bytes"),
        make_entry(1, "sc01", "NARRATION", "かきくけこさ", "Twenty-two byte string"),
        make_entry(2, "sc01", "NARRATION", "しすせそたち", "Sixteen byte row"),
        make_entry(3, "sc01", "NARRATION", "つてとなにぬ", "Thirty-two byte clean text line."),
    };
    const OlsModel zero_var_model = fit_length_regression(zero_var_rows, /*skip_h_scenes=*/true);
    EXPECT_FALSE(zero_var_model.valid);
    EXPECT_EQ(zero_var_model.sample_count, 4U);

    // Exact variance boundary: x = {7, 7, 13, 13} -> sum_xx / 4 == 9.0 (requires > 9.0).
    const std::vector<ScriptEntry> exact_seam_rows = {
        make_entry(0, "sc01", "NARRATION", "あいうえおかき", "Fourteen bytes"),
        make_entry(1, "sc01", "NARRATION", "くけこさしすせ", "Sixteen byte row"),
        make_entry(2, "sc01", "NARRATION", "あいうえおかきくけこさしす", "Twenty-six byte clean line"),
        make_entry(3, "sc01", "NARRATION", "たちつてとなにぬねのはひふ", "Twenty-eight byte clean line"),
    };
    EXPECT_FALSE(fit_length_regression(exact_seam_rows, /*skip_h_scenes=*/true).valid);

    // Above variance boundary: x = {6, 7, 13, 14} -> sum_xx / 4 == 12.5 > 9.0.
    const std::vector<ScriptEntry> above_seam_rows = {
        make_entry(0, "sc01", "NARRATION", "あいうえおか", "Fourteen bytes"),
        make_entry(1, "sc01", "NARRATION", "くけこさしすせ", "Sixteen byte row"),
        make_entry(2, "sc01", "NARRATION", "あいうえおかきくけこさしす", "Twenty-six byte clean line"),
        make_entry(3, "sc01", "NARRATION", "たちつてとなにぬねのはひふへ", "Thirty byte clean text line ok"),
    };
    const OlsModel above_seam_model =
        fit_length_regression(above_seam_rows, /*skip_h_scenes=*/true);
    EXPECT_TRUE(above_seam_model.valid);
    EXPECT_EQ(above_seam_model.sample_count, 4U);
}

TEST(WeaknessSignalsTest, FitsExactHandCalculatedOlsExcludesOutliersAndClampsBounds) {
    // Hand-worked 4-point design:
    //   x = {4, 4, 12, 12}, y = {15, 31, 33, 45}
    //   mean_x = 8, mean_y = 31, sum_xx = 64, sum_xy = 128
    //   beta = 2.0, alpha = 15.0, residuals = {-8, +8, -6, +6}, SSE = 200
    //   unbiased sigma (df = 4 - 2 = 2) = sqrt(200 / 2) = 10.0
    std::vector<ScriptEntry> rows = {
        make_entry(0, "sc01", "NARRATION", "あいうえ", std::string(15, 'a')),
        make_entry(1, "sc01", "NARRATION", "かきくけ", std::string(31, 'b')),
        make_entry(2, "sc01", "NARRATION", "あいうえおかきくけこさし", std::string(33, 'c')),
        make_entry(3, "sc01", "NARRATION", "たちつてとなにぬねのはひ", std::string(45, 'd')),
        make_entry(4, "sc01", "NARRATION", "未翻訳のまま残っている行", "未翻訳のまま残っている行"),
        make_entry(5, "sc01", "NARRATION", "「……」", "\"...\""),
        make_entry(6, "sc01", "NARRATION", "まみむめもやゆよ",
                   std::string(kMaxOlsCleanEnBytes + 1U, 'z')),
    };
    for (std::size_t h = 0; h < kMinSustainedHSceneLines; ++h) {
        rows.push_back(make_entry(
            7U + h, "sc01", (h % 2U == 0U) ? "Yui" : "NARRATION",
            (h % 2U == 0U) ? "「んっ……あんっ……んちゅ、じゅる……」"
                           : "熱い秘裂と蜜壺が愛液に濡れて細かく震え続けた。",
            (h % 2U == 0U)
                ? "\"Nngh... schlick, slurp... wet folds trembling warmly.\""
                : "Slick wetness and hot thrusts echoed softly across the room."));
    }

    const OlsModel model = fit_length_regression(rows, /*skip_h_scenes=*/true);
    ASSERT_TRUE(model.valid);
    EXPECT_EQ(model.sample_count, 4U);
    EXPECT_NEAR(model.alpha, 15.0, kFloatTol);
    EXPECT_NEAR(model.beta, 2.0, kFloatTol);
    EXPECT_NEAR(model.sigma, 10.0, kFloatTol);

    const OlsModel with_h = fit_length_regression(rows, /*skip_h_scenes=*/false);
    ASSERT_TRUE(with_h.valid);
    EXPECT_EQ(with_h.sample_count, 4U + kMinSustainedHSceneLines);

    // Short 4-line (< kMinSustainedHSceneLines) dirty/crude banter is retained in OLS fitting.
    const std::vector<ScriptEntry> rows_with_dirty_banter = {
        rows[0],
        rows[1],
        rows[2],
        rows[3],
        make_entry(4U, "sc01", "Kenji", "「ふざけるな、書類を出せよ！」",
                   "\"Stop acting like a cock and hand over the form!\""),
        make_entry(5U, "sc01", "Yui", "「うるさいわね、少し待ちなさいよ。」",
                   "\"Quit riding my dick and wait five minutes.\""),
        make_entry(6U, "sc01", "Kenji", "「逃げるつもりじゃないだろうな。」",
                   "\"Don't pussy out right before the meeting starts.\""),
        make_entry(7U, "sc01", "Yui", "「さっさと席に着きなさい。」",
                   "\"Sit your sorry ass down in that chair already.\""),
    };
    const OlsModel banter_ols =
        fit_length_regression(rows_with_dirty_banter, /*skip_h_scenes=*/true);
    ASSERT_TRUE(banter_ols.valid);
    EXPECT_EQ(banter_ols.sample_count, 8U);

    const std::vector<ScriptEntry> low_clamp_rows = {
        make_entry(0, "sc01", "NARRATION", "あいうえ", std::string(10, 'a')),
        make_entry(1, "sc01", "NARRATION", "かきくけ", std::string(10, 'b')),
        make_entry(2, "sc01", "NARRATION", "あいうえおかきくけこさし", std::string(12, 'c')),
        make_entry(3, "sc01", "NARRATION", "たちつてとなにぬねのはひ", std::string(12, 'd')),
    };
    const OlsModel low_clamped = fit_length_regression(low_clamp_rows, /*skip_h_scenes=*/true);
    ASSERT_TRUE(low_clamped.valid);
    EXPECT_NEAR(low_clamped.beta, 0.6, kFloatTol);
    EXPECT_NEAR(low_clamped.sigma, 8.0, kFloatTol);

    const std::vector<ScriptEntry> high_clamp_rows = {
        make_entry(0, "sc01", "NARRATION", "あいうえ", std::string(20, 'a')),
        make_entry(1, "sc01", "NARRATION", "かきくけ", std::string(140, 'b')),
        make_entry(2, "sc01", "NARRATION", "あいうえおかきくけこさし", std::string(60, 'c')),
        make_entry(3, "sc01", "NARRATION", "たちつてとなにぬねのはひ", std::string(260, 'd')),
    };
    const OlsModel high_clamped = fit_length_regression(high_clamp_rows, /*skip_h_scenes=*/true);
    ASSERT_TRUE(high_clamped.valid);
    EXPECT_NEAR(high_clamped.beta, 3.0, kFloatTol);
    EXPECT_NEAR(high_clamped.sigma, 40.0, kFloatTol);
}

TEST(WeaknessSignalsTest, ScoresSingleLineDeadbandDroppedClauseAndIsolatedSignals) {
    // 10 Hiragana = 10.0 bits -> expected = 20.0, sigma = 10.0.
    // |z| <= 3.0 holds at 49 bytes (z = 2.9) and 50 bytes (z = 3.0); 51 bytes (z = 3.1) fires.
    const OlsModel deadband_ols = make_pinned_ols(0.0, 2.0, 10.0);
    const std::string ten_hiragana = "あいうえおかきくけこ";
    const std::string prose_51 = "Quiet morning sunlight warmed every wooden corridor";
    ASSERT_EQ(prose_51.size(), 51U);

    EXPECT_NEAR(score_single_line(
                    make_entry(0, "sc01", "NARRATION", ten_hiragana, prose_51.substr(0, 49U)),
                    deadband_ols, /*apply_h_skip=*/true)
                    .total_score,
                0.0, kFloatTol);
    EXPECT_NEAR(score_single_line(
                    make_entry(1, "sc01", "NARRATION", ten_hiragana, prose_51.substr(0, 50U)),
                    deadband_ols, /*apply_h_skip=*/true)
                    .total_score,
                0.0, kFloatTol);
    EXPECT_GT(score_single_line(
                  make_entry(2, "sc01", "NARRATION", ten_hiragana, prose_51),
                  deadband_ols, /*apply_h_skip=*/true)
                  .total_score,
              0.0);

    // Dropped-clause seam: 17.0 bits (< 18.0) vs 18.0 bits (>= 18.0) with sigma = 40.0.
    const OlsModel wide_sigma_ols = make_pinned_ols(0.0, 2.0, 40.0);
    EXPECT_NEAR(score_single_line(
                    make_entry(3, "sc01", "NARRATION",
                               "あいうえおかきくけこさしすせそたち", "Short row."),
                    wide_sigma_ols, /*apply_h_skip=*/true)
                    .total_score,
                0.0, kFloatTol);
    EXPECT_GE(score_single_line(
                  make_entry(4, "sc01", "NARRATION",
                             "あいうえおかきくけこさしすせそたちつ", "Short row."),
                  wide_sigma_ols, /*apply_h_skip=*/true)
                  .total_score,
              3.5);

    struct CleanCase {
        const char* name;
        std::string_view speaker;
        std::string_view jp_text;
        std::string_view en_text;
        bool apply_h_skip;
        bool fix_punctuation;
    };

    constexpr CleanCase kCleanCases[] = {
        {"benefactive_preserved", "Kenji", "「あの時、助けてくれたよな。」",
         "\"Back then, you helped me out.\"", true, false},
        {"first_person_preserved", "Kenji", "「俺は絶対に諦めないぞ。」",
         "\"I will never give up on this.\"", true, false},
        {"yakuwarigo_nya_preserved", "Tama", "「今日はいい天気だにゃ。」",
         "\"Nice weather today, nya.\"", true, false},
        {"colloquial_ore_nya_excluded", "Kenji", "「俺にゃ関係ない話だ。」",
         "\"That has nothing to do with me.\"", true, false},
        {"mimetic_munyamunya_excluded", "Yui", "「むにゃむにゃ……あと五分……」",
         "\"Mumble mumble... five more minutes...\"", true, false},
        {"romaji_gloss_ignored_without_punct_flag", "Yui", "「待ってよ、お兄ちゃん！」",
         "\"Wait up for me (oniisan)!\"", true, false},
        {"quote_mismatch_ignored_without_punct_flag", "Yui", "「おはよう。」",
         "Good morning to everyone.", true, false},
        {"h_scene_skipped_when_apply_h_skip_true", "美咲（Ｈ）", "「あ……駄目……」",
         "\"Ah... 駄目...\"", true, false},
    };

    for (const auto& tc : kCleanCases) {
        SCOPED_TRACE(tc.name);
        const LineWeaknessScore score = score_single_line(
            make_entry(0, "sc01", tc.speaker, tc.jp_text, tc.en_text),
            wide_sigma_ols, tc.apply_h_skip, tc.fix_punctuation);
        EXPECT_NEAR(score.total_score, 0.0, kFloatTol);
        EXPECT_EQ(score.dominant_signal, "clean");
    }

    struct DefectCase {
        const char* name;
        std::string_view speaker;
        std::string_view jp_text;
        std::string_view en_text;
        bool apply_h_skip;
        bool fix_punctuation;
        double min_score;
    };

    constexpr DefectCase kDefectCases[] = {
        {"benefactive_inverted_tekureta", "Kenji", "「あの時、助けてくれたよな。」",
         "\"Back then, I helped you out.\"", true, false, 4.5},
        {"benefactive_inverted_temoratta", "Kenji", "「先輩に教えてもらったんだ。」",
         "\"I taught my senior everything.\"", true, false, 4.5},
        {"first_person_dropped", "Kenji", "「俺は絶対に諦めないぞ。」",
         "\"Giving up is out of the question.\"", true, false, 2.6},
        {"yakuwarigo_nya_dropped", "Tama", "「今日はいい天気だにゃ。」",
         "\"Nice weather out here today.\"", true, false, 2.2},
        {"yakuwarigo_kanya_dropped", "Tama", "「どこへ行ったのかにゃ？」",
         "\"Where did everybody go today?\"", true, false, 2.2},
        {"yakuwarigo_desuwa_dropped", "Reiko", "「素敵なお茶会ですわ。」",
         "\"What a lovely tea party today.\"", true, false, 2.2},
        {"yakuwarigo_degozaru_dropped", "Goemon", "「見事な腕前でござる。」",
         "\"What splendid swordsmanship.\"", true, false, 2.2},
        {"runaway_char_repeat", "Kenji", "「ちょっと待ってくれ！」",
         "\"Wait a seconddddddddddd!\"", true, false, 6.0},
        {"runaway_trigram_loop", "Kenji", "「ここで少し待っていてくれ。」",
         "\"wait right there wait right there wait right there wait right there\"",
         true, false, 7.0},
        {"leaked_japanese", "Kenji", "「今から学園祭へ行こう。」",
         "\"Let's head to 学園祭 right now.\"", true, false, 8.0},
        {"romaji_gloss_with_punct_flag", "Yui", "「待ってよ、お兄ちゃん！」",
         "\"Wait up for me (oniisan)!\"", true, true, 4.0},
        {"quote_mismatch_with_punct_flag", "Yui", "「おはよう。」",
         "Good morning to everyone.", true, true, 3.2},
        {"h_scene_scored_when_apply_h_skip_false", "美咲（Ｈ）", "「あ……駄目……」",
         "\"Ah... 駄目...\"", false, false, 8.0},
    };

    for (const auto& tc : kDefectCases) {
        SCOPED_TRACE(tc.name);
        const LineWeaknessScore score = score_single_line(
            make_entry(0, "sc01", tc.speaker, tc.jp_text, tc.en_text),
            wide_sigma_ols, tc.apply_h_skip, tc.fix_punctuation);
        EXPECT_GE(score.total_score, tc.min_score);
    }
}

TEST(WeaknessSignalsTest, ScoresCorpusQaEchoBlondeLookbackCallbacksDriftAndEntropy) {
    const OlsModel ols = make_pinned_ols(0.0, 2.0, 40.0);
    HotspotConfig cfg;

    // 1. Q-A verb echo: same script broken vs preserved vs different script boundary.
    const std::vector<ScriptEntry> qa_broken_same_script = {
        make_entry(0, "sc01", "Kenji", "「もう鍵は開けた？」", "\"Did you open the lock already?\""),
        make_entry(1, "sc01", "Yui", "「うん、開けたよ。」", "\"Yeah, sure thing.\""),
    };
    const std::vector<ScriptEntry> qa_preserved_same_script = {
        make_entry(0, "sc01", "Kenji", "「もう鍵は開けた？」", "\"Did you open the lock already?\""),
        make_entry(1, "sc01", "Yui", "「うん、開けたよ。」", "\"Yeah, I opened it.\""),
    };
    const std::vector<ScriptEntry> qa_cross_script = {
        make_entry(0, "sc01", "Kenji", "「もう鍵は開けた？」", "\"Did you open the lock already?\""),
        make_entry(1, "sc02", "Yui", "「うん、開けたよ。」", "\"Yeah, sure thing.\""),
    };

    const auto qa_broken_scores = score_corpus_lines(qa_broken_same_script, ols, cfg);
    const auto qa_clean_scores = score_corpus_lines(qa_preserved_same_script, ols, cfg);
    const auto qa_cross_scores = score_corpus_lines(qa_cross_script, ols, cfg);
    ASSERT_EQ(qa_broken_scores.size(), 2U);
    ASSERT_EQ(qa_clean_scores.size(), 2U);
    ASSERT_EQ(qa_cross_scores.size(), 2U);
    EXPECT_GE(qa_broken_scores[1].discourse_score, 2.8);
    EXPECT_NEAR(qa_clean_scores[1].discourse_score, 0.0, kFloatTol);
    EXPECT_NEAR(qa_cross_scores[1].discourse_score, 0.0, kFloatTol);

    // 2. Particle-filtered "俺は" / "私は" do not trigger false callbacks.
    const std::vector<ScriptEntry> pronoun_turns = {
        make_entry(0, "sc01", "Kenji", "「俺は先に行くぞ。」", "\"I am heading on ahead.\""),
        make_entry(1, "sc01", "Kenji", "「俺はここで待つ。」", "\"I will wait right here.\""),
        make_entry(2, "sc01", "Yui", "「私は紅茶にするわ。」", "\"I would like black tea.\""),
        make_entry(3, "sc01", "Yui", "「私は読書が好きよ。」", "\"I enjoy reading novels.\""),
    };
    const auto pronoun_scores = score_corpus_lines(pronoun_turns, ols, cfg);
    ASSERT_EQ(pronoun_scores.size(), 4U);
    for (std::size_t i = 0; i < pronoun_scores.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_NEAR(pronoun_scores[i].discourse_score, 0.0, kFloatTol);
        EXPECT_NEAR(pronoun_scores[i].total_score, 0.0, kFloatTol);
    }

    // 3. Contraction-aware stems: shared "That's" / "didn't" must not mask broken kanji callback.
    const std::vector<ScriptEntry> contraction_masked = {
        make_entry(0, "sc01", "Kenji", "「秘密の約束を覚えているか？」",
                   "\"That's our secret promise, didn't you know?\""),
        make_entry(1, "sc01", "Yui", "「その秘密の約束は忘れない。」",
                   "\"That's why we didn't say anything.\""),
    };
    const std::vector<ScriptEntry> contraction_preserved = {
        make_entry(0, "sc01", "Kenji", "「秘密の約束を覚えているか？」",
                   "\"That's our secret promise, didn't you know?\""),
        make_entry(1, "sc01", "Yui", "「その秘密の約束は忘れない。」",
                   "\"That's why we kept our secret promise.\""),
    };
    const auto masked_scores = score_corpus_lines(contraction_masked, ols, cfg);
    const auto kept_scores = score_corpus_lines(contraction_preserved, ols, cfg);
    ASSERT_EQ(masked_scores.size(), 2U);
    ASSERT_EQ(kept_scores.size(), 2U);
    EXPECT_GE(masked_scores[1].discourse_score, 2.0);
    EXPECT_NEAR(kept_scores[1].discourse_score, 0.0, kFloatTol);

    // 4. 5-turn BlonDe lookback seam: distance 5 ([0, 5) includes 0) vs distance 6 ([1, 6) excludes 0).
    std::vector<ScriptEntry> lookback_5 = {
        make_entry(0, "sc01", "Kenji", "「生徒会の予算を確認する。」",
                   "\"Let's review the student council budget.\""),
        make_entry(1, "sc01", "Yui", "「今日は風が涼しくて気持ちいい。」",
                   "\"The breeze feels cool and pleasant today.\""),
        make_entry(2, "sc01", "Kenji", "「窓の外の雲がゆっくり流れている。」",
                   "\"Clouds outside the window drift slowly by.\""),
        make_entry(3, "sc01", "Yui", "「放課後に少しお茶でも淹れようか。」",
                   "\"Shall we brew a pot of tea after class?\""),
        make_entry(4, "sc01", "Kenji", "「机の上の書類を片付けておこう。」",
                   "\"Let's tidy the papers stacked on the desk.\""),
        make_entry(5, "sc01", "Yui", "「生徒会の予算が足りないわ。」",
                   "\"There simply is not enough money left.\""),
    };
    std::vector<ScriptEntry> lookback_6 = lookback_5;
    lookback_6[5] = make_entry(5, "sc01", "Yui", "「廊下の照明が少し暗い気がする。」",
                               "\"The hallway lights seem slightly dim now.\"");
    lookback_6.push_back(make_entry(6, "sc01", "Yui", "「生徒会の予算が足りないわ。」",
                                    "\"There simply is not enough money left.\""));

    const auto lb5_scores = score_corpus_lines(lookback_5, ols, cfg);
    const auto lb6_scores = score_corpus_lines(lookback_6, ols, cfg);
    ASSERT_EQ(lb5_scores.size(), 6U);
    ASSERT_EQ(lb6_scores.size(), 7U);
    EXPECT_GE(lb5_scores[5].discourse_score, 2.0);
    EXPECT_NEAR(lb6_scores[6].discourse_score, 0.0, kFloatTol);

    // 5. Per-speaker EWMA register/length drift tracked per canonical speaker.
    const char* const kFormalHistoryJp[] = {
        "かきくけこさしすせそなにぬねの",
        "はひふへほまみむめもらりるれろ",
        "きくけこさしすせそなにぬねのは",
        "ひふへほまみむめもらりるれろか",
        "くけこさしすせそなにぬねのはひ",
        "ふへほまみむめもらりるれろかき",
        "けこさしすせそなにぬねのはひふ",
        "へほまみむめもらりるれろかきく",
        "こさしすせそなにぬねのはひふへ",
        "ほまみむめもらりるれろかきくけ",
        "さしすせそなにぬねのはひふへほ",
        "まみむめもらりるれろかきくけこ",
    };
    const char* const kFormalHistoryEn[] = {
        "\"Furthermore, every committee member reviewed that financial charter carefully.\"",
        "\"Consequently, regional directors endorsed each structural amendment yesterday.\"",
        "\"Nevertheless, municipal auditors verified seven independent ledger schedules.\"",
        "\"Accordingly, senior delegates ratified those bilateral diplomatic protocols.\"",
        "\"Moreover, parliamentary counsel documented several regulatory provisions.\"",
        "\"Specifically, academic fellows published comprehensive archival findings.\"",
        "\"Subsequently, provincial magistrates approved additional infrastructure grants.\"",
        "\"Meanwhile, executive secretaries circulated detailed operational guidelines.\"",
        "\"Therefore, departmental supervisors scheduled quarterly compliance briefings.\"",
        "\"Inasmuch, constitutional scholars examined historical precedent thoroughly.\"",
        "\"Likewise, treasury officials balanced annual capital expenditure budgets.\"",
        "\"Ultimately, judicial panelists confirmed unanimous procedural consensus.\"",
    };
    constexpr std::size_t kHistoryTurns = 12;
    std::vector<ScriptEntry> same_speaker_drift;
    for (std::size_t i = 0; i < kHistoryTurns; ++i) {
        same_speaker_drift.push_back(make_entry(
            i, "sc01", "Kenji", kFormalHistoryJp[i], kFormalHistoryEn[i]));
    }
    std::vector<ScriptEntry> other_speaker_turn = same_speaker_drift;
    same_speaker_drift.push_back(make_entry(
        kHistoryTurns, "sc01", "Kenji",
        "なにぬねのはひふへほまみむめも", "\"Yeah, nah.\""));
    other_speaker_turn.push_back(make_entry(
        kHistoryTurns, "sc01", "Taro",
        "なにぬねのはひふへほまみむめも", "\"Yeah, nah.\""));

    const auto drift_same = score_corpus_lines(same_speaker_drift, ols, cfg);
    const auto drift_other = score_corpus_lines(other_speaker_turn, ols, cfg);
    ASSERT_EQ(drift_same.size(), kHistoryTurns + 1U);
    ASSERT_EQ(drift_other.size(), kHistoryTurns + 1U);
    EXPECT_GE(drift_same[kHistoryTurns].total_score, 2.0);
    EXPECT_NEAR(drift_other[kHistoryTurns].total_score, 0.0, kFloatTol);

    // 6. Sliding-window Yule's K monotony and low character-bigram Shannon entropy (< 2.6 bits).
    const char* const kMonotonousLines[] = {
        "Dark shadows filled that quiet room.",
        "Quiet shadows lingered inside that room.",
        "That dark room held quiet shadows.",
        "Shadows stayed quiet inside that dark room.",
        "Inside that quiet room, dark shadows waited.",
        "That quiet dark room had shadows everywhere.",
        "Dark quiet shadows remained in that room.",
        "In that room, quiet dark shadows stayed.",
    };
    constexpr std::size_t kWindowLines = 8;
    std::vector<ScriptEntry> yules_corpus;
    for (std::size_t i = 0; i < kWindowLines; ++i) {
        yules_corpus.push_back(make_entry(
            i, "sc01", "NARRATION", kFormalHistoryJp[i], kMonotonousLines[i]));
    }
    const auto yules_scores = score_corpus_lines(yules_corpus, ols, cfg);
    ASSERT_EQ(yules_scores.size(), kWindowLines);
    EXPECT_GT(yules_scores[kWindowLines - 1U].entropy_score +
                  yules_scores[kWindowLines - 1U].repetition_score,
              0.0);

    const std::vector<ScriptEntry> low_bigram_entropy = {
        make_entry(0, "sc01", "NARRATION", "かきくけこさしすせそなにぬねの",
                   "Anana nanana ananana nananana."),
        make_entry(1, "sc01", "NARRATION", "はひふへほまみむめもらりるれろ",
                   "Quick brown foxes jump nightly."),
    };
    const auto entropy_scores = score_corpus_lines(low_bigram_entropy, ols, cfg);
    ASSERT_EQ(entropy_scores.size(), 2U);
    EXPECT_GT(entropy_scores[0].entropy_score, 0.0);
    EXPECT_NEAR(entropy_scores[1].entropy_score, 0.0, kFloatTol);
}

TEST(WeaknessSignalsTest,
     SustainedHSceneMatrixVsDirtyBanterCombatAndDarkTraumaInCorpusScoring) {
    const OlsModel ols = make_pinned_ols(0.0, 2.0, 40.0);
    HotspotConfig skip_cfg;
    skip_cfg.skip_h_scenes = true;
    HotspotConfig keep_cfg;
    keep_cfg.skip_h_scenes = false;

    // 1. Pure Moan / Fluid Onomatopoeia sustained H-scene (20 lines, untagged speakers):
    constexpr std::size_t kPureMoanLines = 20;
    constexpr const char* kMoanJp[4] = {
        "「んっ……あんっ……んちゅ、ちゅぱ……」",
        "「はぁ……んぅ……じゅる、ぐちゅ……♡」",
        "「ひゃんっ……あぁん……イッちゃう……！」",
        "「くちゅ、ぬちゅ……んっ、ふぁぁ……♥」",
    };
    constexpr const char* kMoanDefectiveEn[4] = {
        "\"Nngh... schlick, slurp... 吐息...\"",
        "\"Hnngh... mmph... squelch, 駄目...\"",
        "\"Aghhhhhhh... nngh... 絶頂...!\"",
        "\"Schlick, slurp... mmph, 快感...\"",
    };
    std::vector<ScriptEntry> pure_moan;
    pure_moan.reserve(kPureMoanLines);
    for (std::size_t i = 0; i < kPureMoanLines; ++i) {
        pure_moan.push_back(make_entry(
            i, "sc_moan", (i % 2U == 0U) ? "Yui" : "Kenji",
            kMoanJp[i % 4U], kMoanDefectiveEn[i % 4U]));
    }
    const auto moan_skip = score_corpus_lines(pure_moan, ols, skip_cfg);
    const auto moan_keep = score_corpus_lines(pure_moan, ols, keep_cfg);
    ASSERT_EQ(moan_skip.size(), kPureMoanLines);
    ASSERT_EQ(moan_keep.size(), kPureMoanLines);
    for (std::size_t i = 0; i < kPureMoanLines; ++i) {
        SCOPED_TRACE(i);
        EXPECT_NEAR(moan_skip[i].total_score, 0.0, kFloatTol);
        EXPECT_EQ(moan_skip[i].dominant_signal, "clean");
        EXPECT_GE(moan_keep[i].total_score, kLeakedJpInEnPenalty);
    }

    // 2. Interleaved 1-Moan/Anchor + 2-Narration Literary Eroge H-scene (22 lines, untagged):
    // Six 2-line plain non-cue narration gaps (<= kMaxHSceneBridgeGapLines) bridged across
    // 10 cue/anchor lines using literary eroge terms (秘裂, 蜜壺, 肉襞, 粘膜, 白濁, 抽挿, 愛液).
    struct InterleavedRow {
        const char* speaker;
        const char* jp;
        const char* en;
    };
    constexpr InterleavedRow kInterleavedTable[] = {
        {"Yui", "「んっ……あんっ……んちゅ、ちゅぱ……」", "\"Nngh... schlick, slurp... 吐息...\""},
        {"NARRATION", "窓の外で夜の雨が静かに降り続いていた。",
         "Night rain fell quietly outside the 窓辺 window."},
        {"NARRATION", "時計の秒針だけが暗い部屋に響いていた。",
         "Only the clock hand echoed inside the 部屋 room."},
        {"NARRATION", "熱い秘裂と蜜壺が愛液に濡れて震えた。",
         "Warm folds and slick wetness trembled in 快感."},
        {"NARRATION", "シーツの皺が指先の下でかすかに揺れた。",
         "Wrinkles in the シーツ shifted beneath fingertips."},
        {"NARRATION", "カーテンの隙間から淡い月明かりが差した。",
         "Pale moonlight slipped through the 窓際 curtain."},
        {"Yui", "「ひゃんっ……ふぁぁ……じゅる、ぐちゅ……♡」",
         "\"Hnngh... mmph... squelch, 駄目...\""},
        {"NARRATION", "枕元のランプが揺れる影を壁に落とした。",
         "The bedside lamp cast swaying 影絵 shadows."},
        {"NARRATION", "遠くの踏切の音が夜気に溶けて消えた。",
         "A distant crossing bell faded into 夜風 air."},
        {"NARRATION", "柔らかな肉襞と粘膜が深い抽挿を受け止めた。",
         "Soft inner walls welcomed each deep thrusting 鼓動."},
        {"NARRATION", "ガラス窓に細い雨粒が幾筋も流れた。",
         "Thin raindrops traced lines down the 硝子 pane."},
        {"NARRATION", "机の上の紅茶はとっくに冷めきっていた。",
         "Black tea on the desk had grown 冷茶 cold."},
        {"Yui", "「あんっ……やぁん……ぬちゅ、くちゅ……」",
         "\"Nngh... aghhhhhh... schlick, 限界...\""},
        {"NARRATION", "木の床がかすかに軋んで静寂を揺らした。",
         "Wooden floorboards creaked softly in 静寂 hush."},
        {"NARRATION", "壁時計の針が深夜二時を静かに指した。",
         "The wall clock pointed quietly to 深夜 two."},
        {"NARRATION", "甘い嬌声と快楽が絡み合い結合を深めた。",
         "Sweet cries and climax pleasure deepened their 抱擁."},
        {"NARRATION", "淡い影が天井の隅でゆっくりと揺れた。",
         "Faint shadows swayed slowly near the 天井 corner."},
        {"NARRATION", "雨音がいっそう強く屋根を叩き始めた。",
         "Raindrops drummed harder against the 屋根 roof."},
        {"NARRATION", "白濁の熱が蜜壺の奥深くへと溢れ出した。",
         "Hot release spilled deep inside her 蜜壺 loins."},
        {"Yui", "「はぁ……はぁ……あっ……」", "\"Haah... haah... aah... 余韻...\""},
        {"Yui", "「んぅ……んちゅ……まだ熱い……」", "\"Mmph... slurp... still 灼熱 warm...\""},
        {"Yui", "「あんっ……愛撫の余韻が消えない……♡」",
         "\"Nngh... warm wetness lingers in 余韻...\""},
    };
    constexpr std::size_t kInterleavedCount =
        sizeof(kInterleavedTable) / sizeof(kInterleavedTable[0]);
    std::vector<ScriptEntry> interleaved;
    interleaved.reserve(kInterleavedCount);
    for (std::size_t i = 0; i < kInterleavedCount; ++i) {
        interleaved.push_back(make_entry(
            i, "sc_interleaved", kInterleavedTable[i].speaker,
            kInterleavedTable[i].jp, kInterleavedTable[i].en));
    }
    const auto interleaved_skip = score_corpus_lines(interleaved, ols, skip_cfg);
    const auto interleaved_keep = score_corpus_lines(interleaved, ols, keep_cfg);
    ASSERT_EQ(interleaved_skip.size(), kInterleavedCount);
    ASSERT_EQ(interleaved_keep.size(), kInterleavedCount);
    for (std::size_t i = 0; i < kInterleavedCount; ++i) {
        SCOPED_TRACE(i);
        EXPECT_NEAR(interleaved_skip[i].total_score, 0.0, kFloatTol);
        EXPECT_EQ(interleaved_skip[i].dominant_signal, "clean");
        EXPECT_GE(interleaved_keep[i].total_score, kLeakedJpInEnPenalty);
    }

    // 3. Speaker-tagged (Ｈ) / (h) sustained H-scene (18 lines, soft dialogue without explicit body):
    constexpr std::size_t kTaggedCount = 18;
    std::vector<ScriptEntry> tagged_h;
    tagged_h.reserve(kTaggedCount);
    for (std::size_t i = 0; i < kTaggedCount; ++i) {
        tagged_h.push_back(make_entry(
            i, "sc_tagged", (i % 2U == 0U) ? "美咲（Ｈ）" : "健司(h)",
            "「このまま朝までずっと傍にいてね……」",
            "\"Stay close beside me until 夜明 morning...\""));
    }
    const auto tagged_skip = score_corpus_lines(tagged_h, ols, skip_cfg);
    const auto tagged_keep = score_corpus_lines(tagged_h, ols, keep_cfg);
    ASSERT_EQ(tagged_skip.size(), kTaggedCount);
    ASSERT_EQ(tagged_keep.size(), kTaggedCount);
    for (std::size_t i = 0; i < kTaggedCount; ++i) {
        SCOPED_TRACE(i);
        EXPECT_NEAR(tagged_skip[i].total_score, 0.0, kFloatTol);
        EXPECT_GE(tagged_keep[i].total_score, kLeakedJpInEnPenalty);
    }

    // 4. Non-H Negative A: 5-line (< kMinSustainedHSceneLines) dirty/crude banter retains full
    // single-line and multi-turn defect scoring even when skip_h_scenes == true:
    const std::vector<ScriptEntry> dirty_banter = {
        make_entry(0, "sc_banter", "Kenji", "「もう鍵は開けた？」",
                   "\"Did you open the damned lock already, you cock?\""),
        make_entry(1, "sc_banter", "Yui", "「うん、開けたよ。」",
                   "\"Quit riding my dick, yeah sure thing!\""),
        make_entry(2, "sc_banter", "Kenji", "「あの時、先輩が助けてくれたよな。」",
                   "\"Back then, I bailed my senior's sorry ass out, don't pussy out.\""),
        make_entry(3, "sc_banter", "Yui", "「今から学園祭へ行こう。」",
                   "\"Cut the crap, cock-up or not let's head to 学園祭 now.\""),
        make_entry(4, "sc_banter", "Kenji", "「俺は絶対に諦めないぞ。」",
                   "\"Giving up on this bastard match is out of the question.\""),
    };
    const auto banter_scores = score_corpus_lines(dirty_banter, ols, skip_cfg);
    ASSERT_EQ(banter_scores.size(), 5U);
    EXPECT_GE(banter_scores[1].discourse_score, kBrokenQaEchoPenalty);
    EXPECT_GE(banter_scores[2].total_score, kBenefactiveInversionPenalty);
    EXPECT_GE(banter_scores[3].total_score, kLeakedJpInEnPenalty);
    EXPECT_GE(banter_scores[4].total_score, kDroppedFirstPersonPenalty);

    // 5. Non-H Negative B: 24-line sustained combat / pain / chase panting scene (0 erotic anchors)
    // is NOT masked even across >= 16 lines when skip_h_scenes == true:
    constexpr std::size_t kCombatCount = 24;
    constexpr const char* kWeapons[6] = {"blade", "spear", "arrow", "dagger", "lance", "steel"};
    std::vector<ScriptEntry> combat_scene;
    combat_scene.reserve(kCombatCount);
    for (std::size_t i = 0; i < kCombatCount; ++i) {
        combat_scene.push_back(make_entry(
            i, "sc_combat", (i % 2U == 0U) ? "Kenji" : "Taro",
            (i % 2U == 0U) ? "「ぐあっ……くっ、剣の傷が深く痛む……！」"
                           : "「はぁ、はぁ……敵の追撃を振り切って走れ！」",
            std::string("\"Gah! Huff, huff... dodge that ") + kWeapons[i % 6U] +
                " and keep running past the bloodied stone wall!\""));
    }
    combat_scene[10] = make_entry(
        10U, "sc_combat", "Kenji", "「ぐあっ……城門まで退却しろ！」",
        "\"Gah! Retreat toward 城門 gate right now!\"");
    combat_scene[15] = make_entry(
        15U, "sc_combat", "Taro", "「くっ……隊長が助けてくれたんだ！」",
        "\"Kuh... I saved the wounded captain from that blade!\"");
    const auto combat_scores = score_corpus_lines(combat_scene, ols, skip_cfg);
    ASSERT_EQ(combat_scores.size(), kCombatCount);
    EXPECT_GE(combat_scores[10].total_score, kLeakedJpInEnPenalty);
    EXPECT_GE(combat_scores[15].total_score, kBenefactiveInversionPenalty);

    // 6. Non-H Negative C: 20-line sustained dark psychological / trauma monologue is NOT masked:
    constexpr std::size_t kTraumaCount = 20;
    constexpr const char* kTraumaNouns[5] = {
        "nightmare", "remorse", "tremor", "isolation", "insomnia",
    };
    std::vector<ScriptEntry> trauma_scene;
    trauma_scene.reserve(kTraumaCount);
    for (std::size_t i = 0; i < kTraumaCount; ++i) {
        trauma_scene.push_back(make_entry(
            i, "sc_trauma", "NARRATION",
            "冷たい汗が震える指先を伝い、昨日の悪夢が脳裏に蘇った。",
            std::string("Cold sweat traced trembling fingers as ") +
                kTraumaNouns[i % 5U] + " echoed through the silent corridor."));
    }
    trauma_scene[8] = make_entry(
        8U, "sc_trauma", "NARRATION", "消えない罪悪感が胸の奥を締め付けた。",
        "Unfading 罪悪感 tightened around a sleepless chest.");
    trauma_scene[14] = make_entry(
        14U, "sc_trauma", "Kenji", "「俺はあの日の約束を忘れられない。」",
        "\"Forgetting that promise from yesterday is impossible.\"");
    const auto trauma_scores = score_corpus_lines(trauma_scene, ols, skip_cfg);
    ASSERT_EQ(trauma_scores.size(), kTraumaCount);
    EXPECT_GE(trauma_scores[8].total_score, kLeakedJpInEnPenalty);
    EXPECT_GE(trauma_scores[14].total_score, kDroppedFirstPersonPenalty);
}

}  // namespace
}  // namespace spot_checker
