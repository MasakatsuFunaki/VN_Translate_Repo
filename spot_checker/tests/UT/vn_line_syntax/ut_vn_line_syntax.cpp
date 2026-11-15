// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

#include "domain/vn_line_syntax.h"
#include "domain/spot_check_types.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace spot_checker {
namespace {

ScriptEntry make_script_entry(std::size_t global_index,
                              std::string_view script_name,
                              std::string_view speaker,
                              std::string_view jp_text,
                              std::string_view current_en) {
    ScriptEntry entry;
    entry.global_index = global_index;
    entry.script_name = std::string(script_name);
    entry.entry_index = global_index;
    entry.type = "DIALOGUE";
    entry.speaker = std::string(speaker);
    entry.jp_text = std::string(jp_text);
    entry.current_en = std::string(current_en);
    return entry;
}

void append_entry(std::vector<ScriptEntry>& entries,
                  std::string_view script_name,
                  std::string_view speaker,
                  std::string_view jp_text,
                  std::string_view current_en) {
    entries.push_back(make_script_entry(
        entries.size(), script_name, speaker, jp_text, current_en));
}

TEST(VnLineSyntax, SplitsSpeakerPrefixAtHeaderByteAndPunctuationBoundaries) {
    const std::string exact_max_speaker(kMaxSpeakerPrefixHeaderBytes, 'K');
    const std::string over_max_speaker(kMaxSpeakerPrefixHeaderBytes + 1U, 'K');

    const SpeakerSplit valid_crlf =
        split_speaker_prefix("Kenichi\r\n\"Good morning.\"");
    EXPECT_EQ(valid_crlf.prefix, "Kenichi\r\n");
    EXPECT_EQ(valid_crlf.speaker_header, "Kenichi");
    EXPECT_EQ(valid_crlf.body, "\"Good morning.\"");
    EXPECT_EQ(extract_speaker_prefix("Kenichi\r\n\"Good morning.\""),
              "Kenichi\r\n");
    EXPECT_EQ(strip_speaker_prefix_body("Kenichi\r\n\"Good morning.\""),
              "\"Good morning.\"");

    const SpeakerSplit valid_exact_limit =
        split_speaker_prefix(exact_max_speaker + "\n「おはよう」");
    EXPECT_EQ(valid_exact_limit.speaker_header, exact_max_speaker);
    EXPECT_EQ(valid_exact_limit.body, "「おはよう」");

    const std::string rejected_inputs[] = {
        over_max_speaker + "\r\n\"Too long.\"",
        "Wait.\nReally?",
        "\"Hello\"\r\n\"World\"",
        "「あ」\n「い」",
        "\r\n\"Missing speaker\"",
        "Kenichi\r\n",
        "Plain single-line narration.",
    };
    for (const std::string& input : rejected_inputs) {
        SCOPED_TRACE(input);
        const SpeakerSplit split = split_speaker_prefix(input);
        EXPECT_EQ(split.prefix, "");
        EXPECT_EQ(split.speaker_header, "");
        EXPECT_EQ(split.body, input);
    }
}

TEST(VnLineSyntax, AnalyzesAndValidatesQuoteEnvelopesAcrossDialogueThoughtsAndSplitBoxes) {
    const QuoteEnvelope cs2_env =
        analyze_quote_envelope("\\n\"Good morning.\"\\@");
    EXPECT_EQ(cs2_env.leading_escape, "\\n");
    EXPECT_EQ(cs2_env.trailing_escape, "\\@");
    EXPECT_EQ(cs2_env.outer_kind, OuterWrapKind::AsciiDoubleQuote);
    EXPECT_EQ(cs2_env.ascii_double_quote_count, 2U);
    EXPECT_TRUE(cs2_env.balanced_ascii_quotes());

    struct CompatCase {
        const char* label;
        std::string_view jp;
        std::string_view old_en;
        std::string_view candidate_en;
        bool expected;
    };
    constexpr CompatCase kCases[] = {
        {"closed_single_corner_preserved", "「おはよう」", "\"Morning.\"",
         "\"Good morning.\"", true},
        {"closed_single_corner_dropped", "「おはよう」", "\"Morning.\"",
         "Good morning.", false},
        {"closed_double_corner_preserved", "『聞こえるか』", "\"Can you hear me?\"",
         "\"Do you copy?\"", true},
        {"restores_dropped_jp_quote", "「本当か？」", "Really?",
         "\"For real?\"", true},
        {"narration_forbids_added_quotes", "風が吹いた。", "The wind blew.",
         "\"The wind blew.\"", false},
        {"thought_parens_preserved", "（まさか……）", "(No way...)",
         "(Could it really be...?)", true},
        {"thought_parens_replaced_with_quotes", "（まさか……）", "(No way...)",
         "\"Could it really be...?\"", false},
        {"catsystem2_escapes_preserved", "「待て！」", "\\n\"Wait!\"\\@",
         "\\n\"Hold it!\"\\@", true},
        {"catsystem2_leading_escape_dropped", "「待て！」", "\\n\"Wait!\"\\@",
         "\"Hold it!\"\\@", false},
        {"catsystem2_trailing_escape_dropped", "「待て！」", "\\n\"Wait!\"\\@",
         "\\n\"Hold it!\"", false},
        {"split_box_open_half_preserved", "「今日から", "\"Since today,",
         "\"Starting today,", true},
        {"split_box_open_half_wrongly_closed", "「今日から", "\"Since today,",
         "\"Starting today.\"", false},
        {"split_box_close_half_preserved", "よろしくね」", "nice to meet you.\"",
         "pleased to meet you.\"", true},
        {"split_box_close_half_wrongly_opened", "よろしくね」", "nice to meet you.\"",
         "\"pleased to meet you.\"", false},
        {"odd_quote_parity_rejected", "「彼は言った」", "\"He spoke.\"",
         "\"He said \"hello.\"", false},
    };

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.label);
        const QuoteEnvelope jp_env = analyze_quote_envelope(tc.jp);
        const QuoteEnvelope old_env = analyze_quote_envelope(tc.old_en);
        const QuoteEnvelope cand_env = analyze_quote_envelope(tc.candidate_en);
        EXPECT_EQ(quote_envelopes_compatible(jp_env, old_env, cand_env),
                  tc.expected);
    }
}

TEST(VnLineSyntax, ExtractsOrderedEngineControlTokensAndStripsInlineWrappers) {
    const std::vector<std::string> tokens = extract_engine_control_tokens(
        "\\n%s gained %02d%% EXP ($str10)$&<12>[r]\r\nNext\\@");
    const std::vector<std::string> expected{
        "\\n", "%s", "%02d", "%%", "$str10", "$&", "<12>", "[r]", "\n", "\\@"};
    ASSERT_EQ(tokens.size(), expected.size());
    EXPECT_EQ(tokens, expected);

    const std::vector<std::string> none =
        extract_engine_control_tokens("100%! Plain English prose.");
    EXPECT_EQ(none.size(), 0U);

    EXPECT_EQ(strip_inline_control_wrappers("<12>\\「おはよう」"), "「おはよう」");
    EXPECT_EQ(strip_inline_control_wrappers("$&「おはよう」"), "「おはよう」");
    EXPECT_EQ(strip_inline_control_wrappers("これは<\"運命|さだめ\">だ"),
              "これは運命だ");
    EXPECT_EQ(strip_inline_control_wrappers("これは[さだめ|運命]だ"),
              "これは運命だ");
}

TEST(VnLineSyntax, DetectsGenericSpeakerHTagsAssetsAndTranslatorNotes) {
    EXPECT_TRUE(has_speaker_h_tag_suffix("太郎Ｈ"));
    EXPECT_TRUE(has_speaker_h_tag_suffix("花子（Ｈ）"));
    EXPECT_TRUE(has_speaker_h_tag_suffix("Taro (h) "));
    EXPECT_TRUE(has_speaker_h_tag_suffix("Hanako h)"));
    EXPECT_FALSE(has_speaker_h_tag_suffix("高橋"));
    EXPECT_FALSE(has_speaker_h_tag_suffix("Hannah"));
    EXPECT_FALSE(has_speaker_h_tag_suffix(""));

    EXPECT_TRUE(is_h_scene_entry("太郎Ｈ", "「おはよう」", "\"Good morning.\""));
    EXPECT_TRUE(is_h_scene_entry(
        "Misaki", "「待って」", "\"Your cock is touching my pussy.\""));
    EXPECT_TRUE(is_h_scene_entry(
        "Misaki", "「ちんぽがまんこに当たってる」", "\"Wait a second.\""));
    EXPECT_FALSE(is_h_scene_entry(
        "Pilot", "操縦席に座った。",
        "The therapist sat in the cockpit reading Dickens."));

    EXPECT_TRUE(is_engine_asset_or_command_token("_ev_01_かな", "_ev_01_かな"));
    EXPECT_TRUE(is_engine_asset_or_command_token("bg/教室_朝", "bg/教室_朝"));
    EXPECT_TRUE(is_engine_asset_or_command_token("se@足音_01", "se@足音_01"));
    EXPECT_TRUE(is_engine_asset_or_command_token("「……」", "\"...\""));
    EXPECT_FALSE(is_engine_asset_or_command_token("「おはよう」", "\"Morning.\""));
    EXPECT_FALSE(is_engine_asset_or_command_token("「おはよう」", "「おはよう」"));

    EXPECT_TRUE(contains_translator_romaji_note("\"Wait up!\" (TL: pun on matsu)"));
    EXPECT_TRUE(contains_translator_romaji_note("[Note: formal speech]"));
    EXPECT_TRUE(contains_translator_romaji_note("\"Big sis!\" (onee-chan)"));
    EXPECT_TRUE(contains_translator_romaji_note("\"Thanks, (senpai)!\""));
    EXPECT_TRUE(contains_translator_romaji_note("He bowed (lit. lowered his head)."));
    EXPECT_FALSE(contains_translator_romaji_note("(I wonder if she noticed me.)"));
}

TEST(VnLineSyntax, NormalizesRestoredTranslationsWithoutDoublingSpeakersOrCorruptingEscapes) {
    ScriptEntry crlf_entry;
    crlf_entry.speaker = "Kenichi";
    crlf_entry.jp_text = "賢一\r\n「おはよう」";
    crlf_entry.current_en = "Kenichi\r\n\"Morning.\"";

    EXPECT_EQ(normalize_restored_translation(crlf_entry,
                                             "[Kenichi] \"Good morning.\""),
              "Kenichi\r\n\"Good morning.\"");
    EXPECT_EQ(normalize_restored_translation(crlf_entry,
                                             "Kenichi: \"Good morning.\""),
              "Kenichi\r\n\"Good morning.\"");
    EXPECT_EQ(normalize_restored_translation(crlf_entry,
                                             "Kenichi\r\n\"Good morning.\""),
              "Kenichi\r\n\"Good morning.\"");

    ScriptEntry inline_entry;
    inline_entry.speaker = "Misaki";
    inline_entry.jp_text = "「おはよう」";
    inline_entry.current_en = "\"Morning.\"";

    EXPECT_EQ(normalize_restored_translation(inline_entry,
                                             "[Misaki] \"Good morning.\""),
              "\"Good morning.\"");
    EXPECT_EQ(normalize_restored_translation(inline_entry,
                                             "Misaki: \"Good morning.\""),
              "\"Good morning.\"");

    ScriptEntry cs2_entry;
    cs2_entry.speaker = "NARRATION";
    cs2_entry.jp_text = "「待て！」";
    cs2_entry.current_en = "\\n\"Wait!\"\\@";

    EXPECT_EQ(normalize_restored_translation(cs2_entry, "\"Hold on!\""),
              "\\n\"Hold on!\"\\@");
    EXPECT_EQ(normalize_restored_translation(cs2_entry, "\\n\"Hold on!\"\\@"),
              "\\n\"Hold on!\"\\@");

    EXPECT_EQ(unescape_llm_newlines("Line 1\\nLine 2", "Old 1\nOld 2"),
              "Line 1\nLine 2");
    EXPECT_EQ(unescape_llm_newlines("\\n\"Keep literal\"", "\\n\"Old literal\""),
              "\\n\"Keep literal\"");
}

TEST(VnLineSyntax, ClassifiesSingleLineHSceneCueTiersAcrossEroticAndCombatBoundaries) {
    struct CueCase {
        const char* label;
        std::string_view speaker;
        std::string_view jp_body;
        std::string_view en_body;
        HSceneCueTier expected_tier;
    };
    constexpr CueCase kCases[] = {
        {"speaker_fullwidth_h_tag_is_explicit_anchor",
         "美咲（Ｈ）",
         "「おはよう」",
         "\"Good morning.\"",
         HSceneCueTier::ExplicitOrTagged},
        {"speaker_ascii_h_tag_is_explicit_anchor",
         "Misaki (h)",
         "「待って」",
         "\"Wait.\"",
         HSceneCueTier::ExplicitOrTagged},
        {"literary_jp_eroge_narration_is_explicit_anchor",
         "NARRATION",
         "秘裂から愛液が溢れ、蜜壺の粘膜が熱く震えた。",
         "Warmth spread rapidly through the dark room.",
         HSceneCueTier::ExplicitOrTagged},
        {"literary_en_eroge_narration_is_explicit_anchor",
         "NARRATION",
         "深く重なり合った。",
         "His shaft pressed against her slick folds toward climax.",
         HSceneCueTier::ExplicitOrTagged},
        {"blunt_en_cock_uppercase_punctuation_boundary_is_explicit_anchor",
         "Misaki",
         "「そんなに押しつけないで！」",
         "\"Don't press your COCK against me!\"",
         HSceneCueTier::ExplicitOrTagged},
        {"blunt_en_vagina_period_boundary_is_explicit_anchor",
         "NARRATION",
         "熱い吐息が重なった。",
         "Heat gathered deep inside her vagina.",
         HSceneCueTier::ExplicitOrTagged},
        {"blunt_en_pussy_clause_boundary_is_explicit_anchor",
         "Misaki",
         "「指が奥まで入ってる……」",
         "\"Your fingers are inside my pussy, wait...\"",
         HSceneCueTier::ExplicitOrTagged},
        {"blunt_en_cum_lowercase_word_boundary_is_explicit_anchor",
         "Kenichi",
         "「もう我慢できない……」",
         "\"I'm going to cum if you keep moving...\"",
         HSceneCueTier::ExplicitOrTagged},
        {"blunt_jp_manko_unspaced_hiragana_is_explicit_anchor",
         "Misaki",
         "「まんこの奥が熱くてたまらないの……」",
         "\"Deep inside me it's burning up...\"",
         HSceneCueTier::ExplicitOrTagged},
        {"blunt_jp_chinpo_unspaced_hiragana_is_explicit_anchor",
         "Misaki",
         "「ちんぽが奥まで届いてる……」",
         "\"It's reaching all the way inside...\"",
         HSceneCueTier::ExplicitOrTagged},
        {"heart_symbol_moan_is_vocal_anchor",
         "Misaki",
         "「だめぇ……♡」",
         "\"Don't...\"",
         HSceneCueTier::EroticVocalOrSfx},
        {"wet_kissing_and_fluid_sfx_is_vocal_anchor",
         "Misaki",
         "「んちゅ……ちゅぱ、じゅるっ……」",
         "\"*slurp* *squelch*\"",
         HSceneCueTier::EroticVocalOrSfx},
        {"nasal_pleasure_moans_are_vocal_anchor",
         "Misaki",
         "「んっ……ふぁ、ひゃんっ！」",
         "\"Nngh... mmph, hnngh!\"",
         HSceneCueTier::EroticVocalOrSfx},
        {"elongated_english_erotic_vocalization_is_vocal_anchor",
         "Misaki",
         "「あぁ……」",
         "\"Aghhhhh... haaahhh... kuhhh...\"",
         HSceneCueTier::EroticVocalOrSfx},
        {"standalone_breath_is_ambiguous_reinforcement",
         "Misaki",
         "「はぁ……はぁ……」",
         "\"Haah... haah...\"",
         HSceneCueTier::AmbiguousBreath},
        {"combat_blade_injury_scream_suppressed_to_none",
         "Kenichi",
         "「ぐあっ！　刃が肩の傷を裂いた！」",
         "\"Gah! The blade tore open the wound on my shoulder!\"",
         HSceneCueTier::None},
        {"chase_running_panting_suppressed_to_none",
         "Kenichi",
         "「はぁ、はぁ……必死に路地を走った！」",
         "\"Huff, huff... I ran desperately down the alley!\"",
         HSceneCueTier::None},
        {"english_false_friend_substrings_remain_none",
         "Pilot",
         "操縦席で書類を読み返した。",
         "The therapist in the cockpit read a document under cumulus clouds with a pussycat.",
         HSceneCueTier::None},
    };

    for (const auto& tc : kCases) {
        SCOPED_TRACE(tc.label);
        EXPECT_EQ(classify_h_scene_cue(tc.speaker, tc.jp_body, tc.en_body),
                  tc.expected_tier);
    }
}

TEST(VnLineSyntax, DetectsSustainedPureVocalizationInterleavedErogeAndScriptIsolatedSpans) {
    constexpr std::size_t kLeadingNormalCount = 10U;
    constexpr std::size_t kPureVocalCount = 26U;
    constexpr std::size_t kTrailingNormalCount = 10U;

    // 1. Pure vocalization / moan / fluid SFX H-scene (no (H) tag, zero anatomical nouns).
    std::vector<ScriptEntry> vocal_corpus;
    for (std::size_t i = 0U; i < kLeadingNormalCount; ++i) {
        append_entry(vocal_corpus, "01_route.ks", "Kenichi",
                     "放課後の教室で宿題を片付けた。",
                     "I finished my homework in the classroom after school.");
    }
    constexpr std::pair<std::string_view, std::string_view> kVocalCycle[] = {
        {"「んっ……あっ、はぁぁ……♡」", "\"Nngh... ahh, haaahhh...\""},
        {"「んちゅ……ちゅぱ、じゅるっ……」", "\"Mmph... *slurp*...\""},
        {"「ふぁ……くぅっ、ひゃんっ！」", "\"Aghhhhh... kuhhh, hnngh!\""},
        {"びゅるっ……ぐちゅ、ぬちゅ、ぴちゃ……", "Wet sounds echoed: *squelch*, *schlick*."},
    };
    constexpr std::size_t kVocalCycleLen = 4U;
    for (std::size_t i = 0U; i < kPureVocalCount; ++i) {
        const auto& line = kVocalCycle[i % kVocalCycleLen];
        append_entry(vocal_corpus, "01_route.ks", "Misaki", line.first, line.second);
    }
    for (std::size_t i = 0U; i < kTrailingNormalCount; ++i) {
        append_entry(vocal_corpus, "01_route.ks", "Kenichi",
                     "翌朝、窓を開けると涼しい風が吹いた。",
                     "The next morning, a cool breeze blew through the open window.");
    }

    const auto vocal_spans = detect_h_scene_spans(vocal_corpus);
    ASSERT_EQ(vocal_spans.size(), 1U);
    EXPECT_EQ(vocal_spans[0], std::make_pair(kLeadingNormalCount, kPureVocalCount));

    const std::vector<bool> vocal_mask = compute_h_scene_line_mask(vocal_corpus);
    ASSERT_EQ(vocal_mask.size(), vocal_corpus.size());
    for (std::size_t i = 0U; i < vocal_mask.size(); ++i) {
        const bool expected = (i >= kLeadingNormalCount &&
                               i < kLeadingNormalCount + kPureVocalCount);
        EXPECT_EQ(vocal_mask[i], expected) << "index=" << i;
    }

    // 2. Interleaved moan dialogue + third-person literary eroge narration + plain
    // atmospheric gaps up to kMaxHSceneBridgeGapLines (4 lines), untagged speakers.
    constexpr std::size_t kInterleavedStart = 8U;
    constexpr std::size_t kInterleavedBlockCount = 4U;
    constexpr std::size_t kMaxGapBlockIndex = 2U;
    constexpr std::size_t kTypicalBridgeGapLines = 2U;
    std::vector<ScriptEntry> interleaved;
    for (std::size_t i = 0U; i < kInterleavedStart; ++i) {
        append_entry(interleaved, "02_night.ks", "NARRATION",
                     "夕食を終えて紅茶を淹れた。",
                     "After dinner, we brewed a pot of black tea.");
    }
    for (std::size_t block = 0U; block < kInterleavedBlockCount; ++block) {
        append_entry(interleaved, "02_night.ks", "Misaki",
                     "「んっ……ふぁ、熱いよぉ……♡」",
                     "\"Nngh... haaahhh, it's so hot...\"");
        append_entry(interleaved, "02_night.ks", "NARRATION",
                     "カーテンの隙間から淡い月明かりが差し込んでいた。",
                     "Pale moonlight filtered through the gap in the curtains.");
        append_entry(interleaved, "02_night.ks", "NARRATION",
                     "秘裂から愛液が溢れ、蜜壺の肉襞が激しく抽挿を受け止める。",
                     "Her slick folds trembled as his shaft pressed deeper toward climax.");
        const std::size_t gap_lines =
            (block == kMaxGapBlockIndex) ? kMaxHSceneBridgeGapLines
                                         : kTypicalBridgeGapLines;
        for (std::size_t g = 0U; g < gap_lines; ++g) {
            append_entry(interleaved, "02_night.ks", "NARRATION",
                         "遠くで時計の針が静かに時を刻んでいた。",
                         "Far away, the hands of the clock ticked quietly.");
        }
    }
    append_entry(interleaved, "02_night.ks", "NARRATION",
                 "白濁を注ぎ込みながら、二人は深い快楽の絶頂へ達した。",
                 "Pleasure surged through their loins in a final climax.");
    const std::size_t expected_interleaved_len =
        interleaved.size() - kInterleavedStart;
    for (std::size_t i = 0U; i < kLeadingNormalCount; ++i) {
        append_entry(interleaved, "02_night.ks", "Misaki",
                     "「おはよう、よく眠れた？」",
                     "\"Good morning, did you sleep well?\"");
    }

    const auto interleaved_spans = detect_h_scene_spans(interleaved);
    ASSERT_EQ(interleaved_spans.size(), 1U);
    EXPECT_EQ(interleaved_spans[0],
              std::make_pair(kInterleavedStart, expected_interleaved_len));

    // 3. Sub-threshold (H)-tagged tails across distinct script_name boundaries
    // never merge across scripts, while a sustained 20-line (H) script is detected.
    constexpr std::size_t kShortHalfCount = 9U;
    constexpr std::size_t kSustainedTaggedCount = 20U;
    std::vector<ScriptEntry> multi_script;
    for (std::size_t i = 0U; i < kShortHalfCount; ++i) {
        append_entry(multi_script, "a_end.ks", "花子（Ｈ）",
                     "「待って……」", "\"Wait...\"");
    }
    for (std::size_t i = 0U; i < kShortHalfCount; ++i) {
        append_entry(multi_script, "b_start.ks", "Hanako (h)",
                     "「まだ……」", "\"Not yet...\"");
    }
    const std::size_t script_c_offset = multi_script.size();
    for (std::size_t i = 0U; i < kSustainedTaggedCount; ++i) {
        append_entry(multi_script, "c_full_h.ks", "花子Ｈ",
                     "「そこは……」", "\"Right there...\"");
    }

    const auto script_spans = detect_h_scene_spans(multi_script);
    ASSERT_EQ(script_spans.size(), 1U);
    EXPECT_EQ(script_spans[0],
              std::make_pair(script_c_offset, kSustainedTaggedCount));

    // 4. Sustained blunt explicit-word H-scene (no hearts, no (H) tag, no literary
    // eroge terms), anchored strictly by blunt EN/JP words interleaved with plain lines.
    constexpr std::pair<std::string_view, std::string_view> kBluntExplicitCycle[] = {
        {"「そんなに押しつけないで……」",
         "\"Don't press your cock against me like that...\""},
        {"「指が奥まで入ってる……」",
         "\"Your fingers are deep inside my pussy...\""},
        {"部屋の隅で小さなランプが静かに揺れていた。",
         "A small lamp flickered quietly in the corner of the room."},
        {"「ちんぽが奥に当たって熱い……」",
         "\"It's pressing deep inside me and burning hot...\""},
        {"浅い呼吸が暗い部屋に重なった。",
         "Heat gathered steadily inside her vagina."},
        {"「まんこの奥が熱くてたまらないの……」",
         "\"Deep inside me it's burning beyond endurance...\""},
        {"「そのまま最後まで離さないで！」",
         "\"Hold me tight until you cum inside me!\""},
    };
    constexpr std::size_t kBluntCycleLen = 7U;
    constexpr std::size_t kBluntExplicitCount = 28U;
    std::vector<ScriptEntry> blunt_corpus;
    for (std::size_t i = 0U; i < kLeadingNormalCount; ++i) {
        append_entry(blunt_corpus, "04_blunt.ks", "Kenichi",
                     "駅前の書店で参考書を一冊買った。",
                     "I bought a study guide at the bookstore in front of the station.");
    }
    for (std::size_t i = 0U; i < kBluntExplicitCount; ++i) {
        const auto& line = kBluntExplicitCycle[i % kBluntCycleLen];
        append_entry(blunt_corpus, "04_blunt.ks", "Misaki", line.first, line.second);
    }
    for (std::size_t i = 0U; i < kTrailingNormalCount; ++i) {
        append_entry(blunt_corpus, "04_blunt.ks", "Kenichi",
                     "朝のホームで電車の到着を待った。",
                     "I waited on the morning platform for the train to arrive.");
    }

    const auto blunt_spans = detect_h_scene_spans(blunt_corpus);
    ASSERT_EQ(blunt_spans.size(), 1U);
    EXPECT_EQ(blunt_spans[0],
              std::make_pair(kLeadingNormalCount, kBluntExplicitCount));

    const std::vector<bool> blunt_mask = compute_h_scene_line_mask(blunt_corpus);
    ASSERT_EQ(blunt_mask.size(), blunt_corpus.size());
    for (std::size_t i = 0U; i < blunt_mask.size(); ++i) {
        const bool expected = (i >= kLeadingNormalCount &&
                               i < kLeadingNormalCount + kBluntExplicitCount);
        EXPECT_EQ(blunt_mask[i], expected) << "index=" << i;
    }
}

TEST(VnLineSyntax, EnforcesExactSpanLengthBridgeGapAnchorAndDensityBoundaries) {
    // 1. Span length seam: 15 (kMinSustainedHSceneLines - 1) vs 16 (kMinSustainedHSceneLines).
    std::vector<ScriptEntry> fifteen_lines;
    for (std::size_t i = 0U; i < kMinSustainedHSceneLines - 1U; ++i) {
        append_entry(fifteen_lines, "len.ks", "Misaki",
                     "「んっ……んちゅ、ふぁぁ……♡」",
                     "\"Nngh... *slurp*, haaahhh...\"");
    }
    EXPECT_EQ(detect_h_scene_spans(fifteen_lines).size(), 0U);

    append_entry(fifteen_lines, "len.ks", "Misaki",
                 "「あんっ……じゅるっ……♡」",
                 "\"Ahhn... *squelch*...\"");
    const auto sixteen_spans = detect_h_scene_spans(fifteen_lines);
    ASSERT_EQ(sixteen_spans.size(), 1U);
    EXPECT_EQ(sixteen_spans[0], std::make_pair(std::size_t{0U}, kMinSustainedHSceneLines));

    // 2. Gap bridge seam: 4 non-cue lines bridged vs 5 non-cue lines split (< 16 each).
    constexpr std::size_t kClusterHalf = 8U;
    auto build_gapped = [kClusterHalf](std::size_t gap_len) {
        std::vector<ScriptEntry> entries;
        for (std::size_t i = 0U; i < kClusterHalf; ++i) {
            append_entry(entries, "gap.ks", "Misaki",
                         "「んっ……ひゃんっ♡」", "\"Nngh... hnngh!\"");
        }
        for (std::size_t i = 0U; i < gap_len; ++i) {
            append_entry(entries, "gap.ks", "NARRATION",
                         "窓の外で静かに雨が降っていた。",
                         "Rain fell quietly outside the window.");
        }
        for (std::size_t i = 0U; i < kClusterHalf; ++i) {
            append_entry(entries, "gap.ks", "Misaki",
                         "「んちゅ……じゅるっ♡」", "\"Mmph... *slurp*!\"");
        }
        return entries;
    };

    const auto bridged_four =
        detect_h_scene_spans(build_gapped(kMaxHSceneBridgeGapLines));
    ASSERT_EQ(bridged_four.size(), 1U);
    EXPECT_EQ(bridged_four[0],
              std::make_pair(std::size_t{0U},
                             kClusterHalf + kClusterHalf + kMaxHSceneBridgeGapLines));

    const auto split_five =
        detect_h_scene_spans(build_gapped(kMaxHSceneBridgeGapLines + 1U));
    EXPECT_EQ(split_five.size(), 0U);

    // 3. Erotic anchor seam: 18 AmbiguousBreath + 2 anchors (< 3) rejected; + 3rd anchor accepted.
    constexpr std::size_t kAmbiguousCount = 18U;
    std::vector<ScriptEntry> anchor_seam;
    for (std::size_t i = 0U; i < kAmbiguousCount; ++i) {
        append_entry(anchor_seam, "anchor.ks", "Misaki",
                     "「はぁ……はぁ……」", "\"Haah... haah...\"");
    }
    for (std::size_t i = 0U; i < kMinSustainedHSceneAnchors - 1U; ++i) {
        append_entry(anchor_seam, "anchor.ks", "Misaki",
                     "「んちゅ……♡」", "\"Nngh...\"");
    }
    EXPECT_EQ(detect_h_scene_spans(anchor_seam).size(), 0U);

    append_entry(anchor_seam, "anchor.ks", "Misaki",
                 "「じゅるっ……♡」", "\"Mmph... *slurp*\"");
    const auto three_anchor_spans = detect_h_scene_spans(anchor_seam);
    ASSERT_EQ(three_anchor_spans.size(), 1U);
    EXPECT_EQ(three_anchor_spans[0],
              std::make_pair(std::size_t{0U}, anchor_seam.size()));

    // 4. Density seam: 7 anchors separated by six 4-line non-cue gaps ->
    // length = 31 >= 16, active = 7 >= 7, anchors = 7 >= 3, density = 7/31 = 0.226 < 0.35.
    std::vector<ScriptEntry> sparse_cluster;
    for (std::size_t c = 0U; c < kMinSustainedHSceneActiveLines; ++c) {
        if (c > 0U) {
            for (std::size_t g = 0U; g < kMaxHSceneBridgeGapLines; ++g) {
                append_entry(sparse_cluster, "sparse.ks", "NARRATION",
                             "廊下の蛍光灯が微かに唸っていた。",
                             "The hallway fluorescent light hummed faintly.");
            }
        }
        append_entry(sparse_cluster, "sparse.ks", "Misaki",
                     "「んちゅ……♡」", "\"Nngh... *slurp*\"");
    }
    EXPECT_EQ(detect_h_scene_spans(sparse_cluster).size(), 0U);
}

TEST(VnLineSyntax, NeverFlagsDirtyBanterSustainedCombatPantingOrDarkPsychologicalScenes) {
    constexpr std::size_t kScenePaddingLines = 12U;
    constexpr std::size_t kSustainedNonHSceneLines = 30U;

    // 1. 5-line burst of blunt explicit dirty jokes inside a normal rooftop comedy scene.
    constexpr std::pair<std::string_view, std::string_view> kDirtyBanterLines[] = {
        {"「昼休みからちんぽだのまんこだの下品な冗談を言うな！」",
         "\"Stop cracking filthy jokes about cock and pussy during lunch!\""},
        {"「保健の授業の単語を連呼して喜んでる小学生かよ。」",
         "\"Are you a grade-schooler giggling because the textbook said vagina?\""},
        {"「購買の練乳パンを見て変な下ネタに繋げるな、最低ね！」",
         "\"Don't compare cafeteria cream bread to cum, you're disgusting!\""},
        {"「ちんぽって大声で叫んだら先生に廊下に立たされるぞ。」",
         "\"Keep yelling cock out loud and the teacher will kick you into the hall.\""},
        {"「まんこだなんて言葉を屋上で口にするんじゃないわよ！」",
         "\"Quit blurting out pussy on the school rooftop already!\""},
    };
    constexpr std::size_t kDirtyBanterCount = 5U;
    std::vector<ScriptEntry> dirty_banter;
    for (std::size_t i = 0U; i < kScenePaddingLines; ++i) {
        append_entry(dirty_banter, "comedy.ks", "Taichi",
                     "屋上のフェンスにもたれて購買のパンをかじった。",
                     "I leaned against the rooftop fence munching cafeteria bread.");
    }
    for (std::size_t i = 0U; i < kDirtyBanterCount; ++i) {
        append_entry(dirty_banter, "comedy.ks", "Taichi",
                     kDirtyBanterLines[i].first, kDirtyBanterLines[i].second);
    }
    for (std::size_t i = 0U; i < kScenePaddingLines; ++i) {
        append_entry(dirty_banter, "comedy.ks", "Misaki",
                     "「まったく、午後の授業に遅れるわよ。」",
                     "\"Honestly, we're going to be late for afternoon class.\"");
    }
    EXPECT_EQ(detect_h_scene_spans(dirty_banter).size(), 0U);
    const std::vector<bool> banter_mask = compute_h_scene_line_mask(dirty_banter);
    ASSERT_EQ(banter_mask.size(), dirty_banter.size());
    for (std::size_t i = 0U; i < banter_mask.size(); ++i) {
        EXPECT_FALSE(banter_mask[i]) << "index=" << i;
    }

    // 2. Sustained 30-line combat / pain / out-of-breath chase scene.
    constexpr std::pair<std::string_view, std::string_view> kCombatCycle[] = {
        {"「ぐあっ！　くそっ、脇腹の傷から血が止まらない！」",
         "\"Gah! Damn it, the wound in my side won't stop bleeding!\""},
        {"「はぁ、はぁ……まだ走れる、追手を振り切るぞ！」",
         "\"Huff, huff... keep running, we'll shake off the pursuers!\""},
        {"「うわあああ！　剣を構えろ、敵の奇襲だ！」",
         "\"Uwaaah! Raise your blade, it's an enemy ambush!\""},
    };
    constexpr std::size_t kCombatCycleLen = 3U;
    std::vector<ScriptEntry> combat_scene;
    for (std::size_t i = 0U; i < kSustainedNonHSceneLines; ++i) {
        const auto& line = kCombatCycle[i % kCombatCycleLen];
        append_entry(combat_scene, "battle.ks", "Kenichi", line.first, line.second);
    }
    EXPECT_EQ(detect_h_scene_spans(combat_scene).size(), 0U);

    // 3. Sustained 30-line dark psychological / trauma / delusion monologue.
    constexpr std::pair<std::string_view, std::string_view> kPsychCycle[] = {
        {"頭蓋の内側で誰かの笑い声が反響し、視界が赤黒く歪んでいく。",
         "Someone's laughter echoed inside my skull as my vision warped dark red."},
        {"「殺したのは僕じゃない……違う、この記憶は偽物だ！」",
         "\"I wasn't the one who killed them... no, this memory is fake!\""},
        {"震える指先で冷たい壁を掻きむしりながら、暗い廊下に崩れ落ちた。",
         "Clawing at the cold wall with trembling fingers, I collapsed in the dark hall."},
    };
    constexpr std::size_t kPsychCycleLen = 3U;
    std::vector<ScriptEntry> psych_scene;
    for (std::size_t i = 0U; i < kSustainedNonHSceneLines; ++i) {
        const auto& line = kPsychCycle[i % kPsychCycleLen];
        append_entry(psych_scene, "denpa.ks", "NARRATION", line.first, line.second);
    }
    EXPECT_EQ(detect_h_scene_spans(psych_scene).size(), 0U);
}

}  // namespace
}  // namespace spot_checker
