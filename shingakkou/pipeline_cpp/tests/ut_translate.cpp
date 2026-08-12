// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 MasakatsuFunaki
//
// Part of VN_TRANSLATE.  Licensed under GPL-3.0-or-later; see LICENSE.
// Not licensed for use as training data for machine learning or generative
// AI systems; text and data mining rights are reserved.  See NOTICE.

// Step-2 tests: the translatability predicates, the speaker lookup pattern,
// response parsing and the prompt shape.  Several of these exist specifically
// to catch a copy-paste from the BLACKCyc ports, whose equivalents differ.
#include <gtest/gtest.h>

#include <string>

#include <boost/json.hpp>

#include "common/util.h"
#include "translate/glossary.h"
#include "translate/translate_core.h"

namespace bj = boost::json;
using namespace shin;
namespace tr = shin::translate;

namespace {

bj::object rec(const char* type, const char* speaker, const char* text = "x") {
    bj::object o;
    o["type"] = type;
    o["speaker"] = speaker;
    o["text"] = text;
    return o;
}

}  // namespace

TEST(Translate, is_control_code_rules) {
    EXPECT_TRUE(tr::is_control_code("何か@です"));            // '@' anywhere wins
    EXPECT_TRUE(tr::is_control_code("chr_a_02"));            // short, no JP punctuation
    EXPECT_FALSE(tr::is_control_code("chr_a_02。"));          // 。 disqualifies
    EXPECT_FALSE(tr::is_control_code("chr_a_02「x」"));
    EXPECT_FALSE(tr::is_control_code("chr_a_02……"));
    EXPECT_FALSE(tr::is_control_code("chr_a_02\xE2\x80\x95"));  // ― U+2015
    EXPECT_FALSE(tr::is_control_code("chr_a_02\xE3\x80\x80"));  // U+3000
    EXPECT_FALSE(tr::is_control_code("no underscore here"));

    // The length bound counts CODEPOINTS.  49 Japanese characters plus '_' is
    // 150 bytes; a byte test would make this false and change the cache keys.
    std::string jp49;
    for (int i = 0; i < 48; ++i) jp49 += "あ";
    EXPECT_EQ(char_len(jp49 + "_"), 49u);
    EXPECT_TRUE(tr::is_control_code(jp49 + "_"));
    std::string jp50 = jp49 + "あ";
    EXPECT_EQ(char_len(jp50 + "_"), 50u);
    EXPECT_FALSE(tr::is_control_code(jp50 + "_"));
}

TEST(Translate, needs_translation_order_and_ideographic_space) {
    // The strip removes U+3000 as well as ASCII whitespace, so an
    // all-ideographic-space line is rejected rather than becoming a junk key.
    EXPECT_FALSE(tr::needs_translation("\xE3\x80\x80\xE3\x80\x80"));
    EXPECT_FALSE(tr::needs_translation(""));
    EXPECT_TRUE(tr::needs_translation("こんにちは"));
    EXPECT_FALSE(tr::needs_translation("Hello"));
    EXPECT_FALSE(tr::needs_translation("……"));
    EXPECT_FALSE(tr::needs_translation("あ@"));  // control-code gate runs first
}

// PATTERN A.  Copying the BLACKCyc extract_speaker (which parses the text)
// would return NARRATION for 100% of lines -- the exact regression the gate
// exists to catch.
TEST(Translate, extract_speaker_reads_the_field_not_the_text) {
    EXPECT_EQ(tr::extract_speaker(rec("dialogue", "マイケル")), "Michael");
    EXPECT_EQ(tr::extract_speaker(rec("narration", "マイケル")), "NARRATION");
    EXPECT_EQ(tr::extract_speaker(rec("dialogue", "")), "NARRATION");
    EXPECT_EQ(tr::extract_speaker(rec("dialogue", "知らない人")), "知らない人");
}

TEST(Translate, parse_numbered_response_strips_a_leading_speaker_tag) {
    // The '&' in the character class is shingakkou-specific; without it the
    // tag stays glued to the front of the translation.
    auto t = tr::parse_numbered_response("[Michael & Gabby] 3. text");
    ASSERT_EQ(t.count(3), 1u);
    EXPECT_EQ(t[3], "text");

    EXPECT_EQ(tr::parse_numbered_response("[NARRATION] 5. hi")[5], "hi");
    EXPECT_EQ(tr::parse_numbered_response("12) foo")[12], "foo");

    // The substitution is ^-anchored and runs BEFORE the number match, so a
    // tag AFTER the number is deliberately kept -- there it is part of the
    // translated line, not a wrapper around it.
    EXPECT_EQ(tr::parse_numbered_response("3. [NARRATION] text")[3], "[NARRATION] text");
}

// The BLACKCyc pipelines un-escape \r\n here; shingakkou must NOT, because a
// real CR/LF in the value would break the TSV row it ends up in.
TEST(Translate, parse_numbered_response_does_not_unescape_newlines) {
    const auto t = tr::parse_numbered_response("1. a\\r\\nb");
    ASSERT_EQ(t.count(1), 1u);
    // Six characters: a, backslash, r, backslash, n, b -- no CR and no LF.
    EXPECT_EQ(t.at(1), "a\\r\\nb");
    EXPECT_EQ(t.at(1).size(), 6u);
    EXPECT_EQ(t.at(1).find('\r'), std::string::npos);
    EXPECT_EQ(t.at(1).find('\n'), std::string::npos);
}

TEST(Translate, sanitize_ascii_eight_replacements) {
    EXPECT_EQ(tr::sanitize_ascii("a\xE2\x80\x94""b"), "a--b");        // em dash
    EXPECT_EQ(tr::sanitize_ascii("a\xE2\x80\x93""b"), "a--b");        // en dash
    EXPECT_EQ(tr::sanitize_ascii("a\xE2\x80\xA6""b"), "a...b");       // ellipsis
    EXPECT_EQ(tr::sanitize_ascii("\xE2\x80\x98x\xE2\x80\x99"), "'x'");
    EXPECT_EQ(tr::sanitize_ascii("\xE2\x80\x9Cx\xE2\x80\x9D"), "\"x\"");
    EXPECT_EQ(tr::sanitize_ascii("a\xE3\x80\x80""b"), "a b");         // U+3000
}

// Re-attaching a name plate here (as the BLACKCyc ports do) would double it:
// step 3 already prepends one, giving "Michael\nMichael\nHello" at runtime.
TEST(Translate, postprocess_does_not_reattach_a_speaker) {
    EXPECT_EQ(tr::postprocess("マイケル\\n「x」", "Hello"), "Hello");
    EXPECT_EQ(tr::postprocess("あ", "あ"), "あ");   // identical -> untouched
    EXPECT_EQ(tr::postprocess("あ", ""), "");
}

TEST(Translate, build_user_prompt_shape) {
    const std::vector<std::pair<std::string, std::string>> batch = {{"Michael", "あ"},
                                                                    {"NARRATION", "い"}};
    const std::string p = tr::build_user_prompt(batch, {});
    // No context BLOCK at all when the window is empty.  (The tail sentence
    // mentions <previous_context> in prose, so search for the opening tag.)
    EXPECT_NE(p.rfind("<lines_to_translate>\n", 0), std::string::npos);
    EXPECT_EQ(p.find("<previous_context>\n"), std::string::npos);
    EXPECT_EQ(p.find("</previous_context>"), std::string::npos);
    EXPECT_NE(p.find("1. [Michael] あ\n2. [NARRATION] い\n</lines_to_translate>"),
              std::string::npos);
    EXPECT_NE(p.find("Translate the 2 lines inside"), std::string::npos);
    EXPECT_NE(p.find("Output EXACTLY 2 numbered translations"), std::string::npos);

    // Line text goes in RAW: a literal backslash-n stays two characters.
    const std::string raw =
        tr::build_user_prompt({{"Michael", "a\\r\\nb"}}, {});
    EXPECT_NE(raw.find("1. [Michael] a\\r\\nb\n"), std::string::npos);
}

TEST(Translate, build_user_prompt_context_window_and_truncation) {
    std::vector<std::pair<std::string, std::string>> ctx;
    for (int i = 0; i < 60; ++i) ctx.emplace_back("S", "line" + std::to_string(i));
    const std::string p = tr::build_user_prompt({{"Michael", "x"}}, ctx);
    // Last CONTEXT_WINDOW (50) entries only: line0..line9 are dropped.
    EXPECT_EQ(p.find("[S] line9\n"), std::string::npos);
    EXPECT_NE(p.find("[S] line10\n"), std::string::npos);
    EXPECT_NE(p.find("[S] line59\n"), std::string::npos);

    // text[:100] is a CODEPOINT slice.  120 Japanese characters is 360 bytes;
    // a byte cut would keep 33 characters and split the 34th mid-sequence.
    std::string jp120;
    for (int i = 0; i < 120; ++i) jp120 += "あ";
    std::string want;
    for (int i = 0; i < 100; ++i) want += "あ";
    const std::string q = tr::build_user_prompt({{"M", "x"}}, {{"S", jp120}});
    EXPECT_NE(q.find("[S] " + want + "\n"), std::string::npos);
}

// This is what lets the 75 glossary names be re-seeded at the top of every run
// without hoisting them to the front of the 6.6 MB cache file.
TEST(Translate, cache_preserves_insertion_order_across_reseed) {
    tr::Cache c;
    c.set("A", "1");
    c.set("B", "2");
    c.set("C", "3");
    c.set("A", "9");
    ASSERT_EQ(c.size(), 3u);
    EXPECT_EQ(c.items()[0].first, "A");
    EXPECT_EQ(c.items()[0].second, "9");
    EXPECT_EQ(c.items()[1].first, "B");
    EXPECT_EQ(c.items()[2].first, "C");
}

TEST(Translate, glossary_has_the_expected_shape) {
    EXPECT_EQ(tr::name_translations_ordered().size(), 75u);
    EXPECT_EQ(tr::name_translations().size(), 75u);
    EXPECT_EQ(tr::story_order().size(), 91u);
    EXPECT_EQ(tr::CONTEXT_WINDOW, 50);  // NOT the template's 10
    EXPECT_EQ(tr::name_translations().at("マイケル・ガビィ"), "Michael & Gabby");
}
