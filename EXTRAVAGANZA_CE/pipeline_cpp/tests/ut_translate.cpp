// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Step-2 tests: speaker parsing off the raw line text, postprocess's
// name-plate re-attachment, prompt shape and response parsing.
#include <gtest/gtest.h>

#include <string>

#include "common/util.h"
#include "translate/glossary.h"
#include "translate/translate_core.h"

namespace tr = exc::translate;
using namespace exc;

namespace {
constexpr const char* MIYAKA = "\xE7\xBE\x8E\xE5\xBC\xA5\xE9\xA6\x99";  // 美弥香
constexpr const char* OPEN = "\xE3\x80\x8C";                            // 「
constexpr const char* CLOSE = "\xE3\x80\x8D";                           // 」
}  // namespace

TEST(Translate, has_real_japanese_excludes_fullwidth) {
    EXPECT_TRUE(tr::has_real_japanese("\xE3\x81\x82"));   // あ
    EXPECT_TRUE(tr::has_real_japanese(MIYAKA));
    // Fullwidth Latin and CJK punctuation are NOT "real Japanese" here, even
    // though the extractor's wider test accepts them.
    EXPECT_FALSE(tr::has_real_japanese("\xEF\xBC\xA1"));  // Ａ
    EXPECT_FALSE(tr::has_real_japanese("\xE3\x80\x8C"));  // 「
}

TEST(Translate, needs_translation_rules) {
    EXPECT_TRUE(tr::needs_translation("\xE3\x81\x82\xE3\x81\x84"));
    EXPECT_FALSE(tr::needs_translation(""));
    EXPECT_FALSE(tr::needs_translation("   "));
    EXPECT_FALSE(tr::needs_translation("abc"));
    EXPECT_FALSE(tr::needs_translation("abcd"));  // no real Japanese at all
}

TEST(Translate, extract_speaker_needs_a_quote_bracket) {
    // Name + bracketed dialogue -> resolved speaker.
    EXPECT_EQ(tr::extract_speaker(std::string(MIYAKA) + "\r\n" + OPEN + "\xE3\x81\x82" + CLOSE),
              "Miyaka");
    // Two-line NARRATIVE (no bracket) must not be mistaken for a speaker --
    // this is exactly the mis-tag the bracket gate was added to prevent.
    EXPECT_EQ(tr::extract_speaker("\xE3\x81\x9D\xE3\x81\xAE\xE4\xB8\x80\xE5\xBF\x83\xE3\x81\x8B"
                                  "\xE3\x82\x89\xE3\x80\x81\r\n\xE7\xBE\x8E\xE5\xBC\xA5\xE9\xA6"
                                  "\x99\xE3\x81\xAF"),
              "NARRATION");
    // No CRLF at all.
    EXPECT_EQ(tr::extract_speaker("\xE3\x81\x82"), "NARRATION");
    // Unknown JP name passes through verbatim.
    EXPECT_EQ(tr::extract_speaker(std::string("\xE6\x9C\xAA\xE7\x9F\xA5") + "\r\n" + OPEN + "x" +
                                  CLOSE),
              "\xE6\x9C\xAA\xE7\x9F\xA5");
}

TEST(Translate, sanitize_ascii_replaces_smart_punctuation) {
    EXPECT_EQ(tr::sanitize_ascii("a\xE2\x80\x94" "b"), "a--b");
    EXPECT_EQ(tr::sanitize_ascii("\xE2\x80\x9C" "hi\xE2\x80\x9D"), "\"hi\"");
    EXPECT_EQ(tr::sanitize_ascii("\xE2\x80\xA6"), "...");
    EXPECT_EQ(tr::sanitize_ascii("a\xE3\x80\x80" "b"), "a b");
}

// The engine renders the text before the first CRLF as the name plate, so the
// English name has to be re-attached there.
TEST(Translate, postprocess_reattaches_english_name_plate) {
    const std::string original = std::string(MIYAKA) + "\r\n" + OPEN + "\xE3\x81\x82" + CLOSE;
    EXPECT_EQ(tr::postprocess(original, "\"Ah\""), "Miyaka\r\n\"Ah\"");
    // If Claude kept the Japanese plate, it is swapped rather than duplicated.
    EXPECT_EQ(tr::postprocess(original, std::string(MIYAKA) + "\r\n\"Ah\""),
              "Miyaka\r\n\"Ah\"");
    // Already-correct output is left alone.
    EXPECT_EQ(tr::postprocess(original, "Miyaka\r\n\"Ah\""), "Miyaka\r\n\"Ah\"");
}

TEST(Translate, postprocess_restores_trailing_crlf) {
    const std::string original = "\xE3\x81\x82\r\n";
    EXPECT_EQ(tr::postprocess(original, "Ah"), "Ah\r\n");
    EXPECT_EQ(tr::postprocess(original, "Ah\r\n"), "Ah\r\n");
}

TEST(Translate, postprocess_passes_empty_and_identical_through) {
    EXPECT_EQ(tr::postprocess("\xE3\x81\x82", ""), "");
    EXPECT_EQ(tr::postprocess("\xE3\x81\x82", "\xE3\x81\x82"), "\xE3\x81\x82");
}

TEST(Translate, user_prompt_escapes_newlines_and_tags_speakers) {
    const std::string p = tr::build_user_prompt({{"Miyaka", "a\r\nb"}}, {});
    EXPECT_NE(p.find("1. [Miyaka] a\\r\\nb\n"), std::string::npos);
    EXPECT_NE(p.find("Do NOT include [SPEAKER] tags"), std::string::npos);
    EXPECT_EQ(p.find("</previous_context>"), std::string::npos);
}

TEST(Translate, context_window_is_capped_at_ten) {
    std::vector<std::pair<std::string, std::string>> ctx;
    for (int i = 0; i < tr::CONTEXT_WINDOW + 5; ++i)
        ctx.emplace_back("S", "line" + std::to_string(i));
    const std::string p = tr::build_user_prompt({{"S", "x"}}, ctx);
    EXPECT_EQ(p.find("[S] line0\n"), std::string::npos) << "oldest line leaked in";
    EXPECT_NE(p.find("[S] line5\n"), std::string::npos) << "window start missing";
    EXPECT_NE(p.find("[S] line14\n"), std::string::npos) << "newest line missing";
}

// The response parser un-escapes \r\n so the engine gets real line breaks.
TEST(Translate, parses_numbered_response_and_unescapes) {
    auto t = tr::parse_numbered_response("1. Miyaka\\r\\n\"Ah\"\n2) Second\n");
    ASSERT_EQ(t.size(), 2u);
    EXPECT_EQ(t[1], "Miyaka\r\n\"Ah\"");
    EXPECT_EQ(t[2], "Second");
}

TEST(Translate, parses_and_strips_leaked_speaker_tag) {
    auto t = tr::parse_numbered_response("[NARRATION] 1. Snow fell.\n");
    ASSERT_EQ(t.size(), 1u);
    EXPECT_EQ(t[1], "Snow fell.");
}

TEST(Translate, glossary_and_story_order_are_well_formed) {
    const auto& ordered = tr::name_translations_ordered();
    EXPECT_FALSE(ordered.empty());
    for (const auto& [jp, en] : ordered) EXPECT_FALSE(jp.empty());
    EXPECT_EQ(ordered.size(), tr::name_translations().size())
        << "duplicate JP key in the glossary";
    EXPECT_FALSE(tr::story_order().empty());
}

// ---------------------------------------------------------------------------
// EXTRAVAGANZA CE only: the speaker lookup tolerates the 1-3 control bytes SPT
// extraction sometimes prepends to a name, via a longest-suffix fallback.
// Both regressions below are silent in-game -- the line still translates, it
// just loses its name plate -- so they are pinned here rather than left to be
// spotted by eye in translated_text.json.
// ---------------------------------------------------------------------------

TEST(Translate, match_speaker_name_exact_then_longest_suffix) {
    // Exact hit.
    ASSERT_TRUE(tr::match_speaker_name(u8"夢美").has_value());
    EXPECT_EQ(*tr::match_speaker_name(u8"夢美"), "Yumemi");
    // Control-byte-corrupted name still resolves by suffix.
    ASSERT_TRUE(tr::match_speaker_name(u8"ﾗ夢美").has_value());
    EXPECT_EQ(*tr::match_speaker_name(u8"ﾗ夢美"), "Yumemi");
    // A longer JP name whose tail is a known name resolves to that name.
    ASSERT_TRUE(tr::match_speaker_name(u8"南優斗").has_value());
    EXPECT_EQ(*tr::match_speaker_name(u8"南優斗"), "Yuuto");
    // No suffix in common -> no match at all.
    EXPECT_FALSE(tr::match_speaker_name(u8"まったく未知").has_value());
}

// The name plate is attached even when the padded/prefixed form is NOT a
// glossary key -- a plain dictionary lookup here dropped the plate on every
// such line.
TEST(Translate, postprocess_attaches_plate_via_suffix_match) {
    const std::string original = u8"\u3000\u3000南優斗\r\n";
    EXPECT_EQ(tr::postprocess(original, "Minami Yuto\r\n"), "Yuuto\r\nMinami Yuto\r\n");
}

TEST(Translate, extract_speaker_uses_suffix_match_and_keeps_unknown_names) {
    EXPECT_EQ(tr::extract_speaker(u8"ﾗ夢美\r\n「あ」"), "Yumemi");
    // Structurally a speaker but unknown -> returned verbatim, not NARRATION.
    EXPECT_EQ(tr::extract_speaker(u8"まったく未知\r\n「あ」"), u8"まったく未知");
    // No quote bracket -> prose, not a speaker.
    EXPECT_EQ(tr::extract_speaker(u8"夢美\r\nprose"), "NARRATION");
}

// The rolling-context truncation feeds build_user_prompt -- the REAL request
// body -- so it must cut by codepoints, not by bytes.  A byte cut both
// shortens the context and can split a multi-byte sequence, and boost::json
// does not validate UTF-8 on the way out.
TEST(Translate, context_window_truncates_by_codepoints_not_bytes) {
    // 60 three-byte characters = 180 bytes; a 100-CHARACTER cut keeps all 60.
    std::string jp;
    for (int i = 0; i < 60; ++i) jp += u8"あ";
    const std::string p = tr::build_user_prompt({{"S", "x"}}, {{"S", jp}});

    const std::size_t open = p.find("[S] ");
    ASSERT_NE(open, std::string::npos);
    const std::size_t eol = p.find('\n', open);
    ASSERT_NE(eol, std::string::npos);
    const std::string ctx = p.substr(open + 4, eol - open - 4);

    std::size_t chars = 0, i = 0;
    while (i < ctx.size()) { utf8_next(ctx, i); ++chars; }
    EXPECT_EQ(chars, 60u) << "context was truncated by bytes, not characters";

    // Whatever survives must still be well-formed UTF-8 (no split sequence).
    EXPECT_EQ(ctx.size() % 3, 0u);
}
